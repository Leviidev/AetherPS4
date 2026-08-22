// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/runtime_client.h"

#include <cerrno>
#include <atomic>
#include <cstring>
#include <limits>
#include <sstream>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Platform::Bachata {
namespace {

std::atomic<RuntimeClient*> active_runtime_client{};

constexpr std::string_view Prefix = "BACHATA/1 ";

bool IsInsideRoot(const std::filesystem::path& child, const std::filesystem::path& root) {
    const auto normalized_child = child.lexically_normal();
    const auto normalized_root = root.lexically_normal();
    auto child_it = normalized_child.begin();
    for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
         ++root_it, ++child_it) {
        if (child_it == normalized_child.end() || *child_it != *root_it) {
            return false;
        }
    }
    return true;
}

std::optional<int> ParseInt(std::string_view value) {
    int parsed = 0;
    if (value.empty()) {
        return std::nullopt;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed = (parsed * 10) + (c - '0');
    }
    return parsed;
}

} // namespace

std::optional<RuntimeFrame> ParseRuntimeFrame(std::string_view line) {
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
    }
    if (!line.starts_with(Prefix)) {
        return std::nullopt;
    }

    std::istringstream input{std::string(line.substr(Prefix.size()))};
    std::string kind;
    input >> kind;
    if (kind == "HELLO") {
        std::string version;
        input >> version;
        if (!version.starts_with("version=")) {
            return std::nullopt;
        }
        const auto parsed = ParseInt(std::string_view(version).substr(8));
        if (!parsed || *parsed != static_cast<int>(RuntimeProtocolVersion)) {
            return std::nullopt;
        }
        return RuntimeFrame{RuntimeEvent::Hello, RuntimeProtocolVersion};
    }

    if (kind != "EVENT" && kind != "ERROR") {
        return std::nullopt;
    }

    std::string value;
    input >> value;
    if (kind == "ERROR") {
        if (!value.starts_with("code=") || value.size() == 5) {
            return std::nullopt;
        }
        return RuntimeFrame{RuntimeEvent::Error, RuntimeProtocolVersion, 0, value.substr(5)};
    }

    if (value == "Starting") {
        return RuntimeFrame{RuntimeEvent::Starting, RuntimeProtocolVersion};
    }
    if (value == "Running") {
        return RuntimeFrame{RuntimeEvent::Running, RuntimeProtocolVersion};
    }
    if (value == "Stopped") {
        std::string exit_code;
        input >> exit_code;
        if (!exit_code.starts_with("exit_code=")) {
            return std::nullopt;
        }
        const auto parsed = ParseInt(std::string_view(exit_code).substr(10));
        if (!parsed) {
            return std::nullopt;
        }
        return RuntimeFrame{RuntimeEvent::Stopped, RuntimeProtocolVersion, *parsed};
    }

    return std::nullopt;
}

bool ValidateSocketPath(const std::filesystem::path& socket_path,
                        const std::filesystem::path& runtime_root) {
    return socket_path.is_absolute() && runtime_root.is_absolute() &&
           IsInsideRoot(socket_path, runtime_root);
}

RuntimeClient::RuntimeClient(int fd) : fd_(fd) {}

RuntimeClient::RuntimeClient(RuntimeClient&& other) noexcept
    : fd_(other.fd_), input_thread_(std::move(other.input_thread_)) {
    other.fd_ = -1;
}

RuntimeClient& RuntimeClient::operator=(RuntimeClient&& other) noexcept {
    if (this != &other) {
        StopInputReader();
        Close();
        fd_ = other.fd_;
        input_thread_ = std::move(other.input_thread_);
        other.fd_ = -1;
    }
    return *this;
}

RuntimeClient::~RuntimeClient() {
    StopInputReader();
    Close();
}

RuntimeClient RuntimeClient::Disabled() {
    return RuntimeClient{};
}

std::optional<RuntimeClient> RuntimeClient::Connect(const std::filesystem::path& socket_path) {
#ifdef _WIN32
    (void)socket_path;
    return std::nullopt;
#else
    const auto native = socket_path.string();
    if (native.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, native.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return RuntimeClient(fd);
#endif
}

bool RuntimeClient::IsEnabled() const {
    return fd_ >= 0;
}

int RuntimeClient::NativeHandleForTest() const {
    return fd_;
}

bool RuntimeClient::SendHello(unsigned version) {
    return SendLine("BACHATA/1 HELLO version=" + std::to_string(version) + "\n");
}

bool RuntimeClient::SendStarting() {
    return SendLine("BACHATA/1 EVENT Starting\n");
}

bool RuntimeClient::SendRunning() {
    return SendLine("BACHATA/1 EVENT Running\n");
}

bool RuntimeClient::SendFramePresented() {
    return SendLine("BACHATA/1 EVENT Frame\n");
}

void SetActiveRuntimeClient(RuntimeClient* client) {
    active_runtime_client.store(client, std::memory_order_release);
}

void ReportPresentedFrame() {
    if (auto* client = active_runtime_client.load(std::memory_order_acquire); client != nullptr) {
        client->SendFramePresented();
    }
}

bool RuntimeClient::SendStopped(int exit_code) {
    return SendLine("BACHATA/1 EVENT Stopped exit_code=" + std::to_string(exit_code) + "\n");
}

bool RuntimeClient::SendError(std::string_view code) {
    return SendLine("BACHATA/1 ERROR code=" + std::string(code) + "\n");
}

bool RuntimeClient::StartInputReader(std::function<void(const ControllerSnapshot&)> handler) {
    if (!IsEnabled() || input_thread_.joinable() || !handler) {
        return false;
    }
#ifdef _WIN32
    return false;
#else
    const int input_fd = fd_;
    input_thread_ = std::jthread([input_fd, handler = std::move(handler)](std::stop_token stop) {
        constexpr std::size_t MaxInputFrameBytes = 512;
        std::string line;
        std::array<std::uint64_t, 4> last_sequence{};
        std::array<bool, 4> received{};
        bool overflow = false;
        char value = 0;
        while (!stop.stop_requested()) {
            const ssize_t count = ::recv(input_fd, &value, 1, 0);
            if (count == 0) {
                break;
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (value != '\n') {
                if (line.size() < MaxInputFrameBytes) {
                    line.push_back(value);
                } else {
                    overflow = true;
                }
                continue;
            }
            if (!overflow) {
                line.push_back('\n');
                const auto snapshot = ParseControllerSnapshot(line);
                if (snapshot && (!received[snapshot->slot] || snapshot->sequence > last_sequence[snapshot->slot])) {
                    handler(*snapshot);
                    last_sequence[snapshot->slot] = snapshot->sequence;
                    received[snapshot->slot] = true;
                }
            }
            line.clear();
            overflow = false;
        }
        for (int slot = 0; slot < 4; ++slot) {
            if (received[slot]) {
                ControllerSnapshot neutral{};
                neutral.slot = slot;
                neutral.sequence = last_sequence[slot] == std::numeric_limits<std::uint64_t>::max()
                                       ? last_sequence[slot]
                                       : last_sequence[slot] + 1;
                handler(neutral);
            }
        }
    });
    return true;
#endif
}

void RuntimeClient::StopInputReader() {
    if (!input_thread_.joinable()) {
        return;
    }
    input_thread_.request_stop();
#ifndef _WIN32
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RD);
    }
#endif
    input_thread_.join();
}

bool RuntimeClient::SendLine(std::string_view line) {
    if (!IsEnabled()) {
        return true;
    }

#ifdef _WIN32
    return false;
#else
    std::scoped_lock lock{send_mutex_};
    const char* data = line.data();
    size_t remaining = line.size();
    while (remaining > 0) {
#ifdef MSG_NOSIGNAL
        const ssize_t written = ::send(fd_, data, remaining, MSG_NOSIGNAL);
#else
        const ssize_t written = ::send(fd_, data, remaining, 0);
#endif
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
#endif
}

void RuntimeClient::Close() {
#ifndef _WIN32
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

} // namespace Platform::Bachata
