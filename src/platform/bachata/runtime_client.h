// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>

#include "platform/bachata/controller_snapshot.h"

namespace Platform::Bachata {

constexpr unsigned RuntimeProtocolVersion = 1;

enum class RuntimeEvent {
    Hello,
    Starting,
    Running,
    Stopped,
    Error,
};

struct RuntimeFrame {
    RuntimeEvent event{};
    unsigned version = RuntimeProtocolVersion;
    int exit_code = 0;
    std::string code;
};

std::optional<RuntimeFrame> ParseRuntimeFrame(std::string_view line);

bool ValidateSocketPath(const std::filesystem::path& socket_path,
                        const std::filesystem::path& runtime_root);

class RuntimeClient {
public:
    RuntimeClient() = default;
    explicit RuntimeClient(int fd);
    RuntimeClient(const RuntimeClient&) = delete;
    RuntimeClient& operator=(const RuntimeClient&) = delete;
    RuntimeClient(RuntimeClient&& other) noexcept;
    RuntimeClient& operator=(RuntimeClient&& other) noexcept;
    ~RuntimeClient();

    static RuntimeClient Disabled();
    static std::optional<RuntimeClient> Connect(const std::filesystem::path& socket_path);

    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] int NativeHandleForTest() const;

    bool SendHello(unsigned version = RuntimeProtocolVersion);
    bool SendStarting();
    bool SendRunning();
    bool SendFramePresented();
    bool SendStopped(int exit_code);
    bool SendError(std::string_view code);
    bool StartInputReader(std::function<void(const ControllerSnapshot&)> handler);
    void StopInputReader();

private:
    bool SendLine(std::string_view line);
    void Close();

    int fd_ = -1;
    std::jthread input_thread_;
    std::mutex send_mutex_;
};

void SetActiveRuntimeClient(RuntimeClient* client);

#ifdef ENABLE_BACHATA_RUNTIME
void ReportPresentedFrame();
#else
// driver.cpp calls this unconditionally on every present regardless of build mode; only the
// implementation (runtime_client.cpp) is gated behind ENABLE_BACHATA_RUNTIME, so provide a
// no-op here rather than scattering #ifdefs at every call site.
inline void ReportPresentedFrame() {}
#endif

} // namespace Platform::Bachata
