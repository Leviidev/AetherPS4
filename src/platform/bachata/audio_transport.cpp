// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/audio_transport.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Platform::Bachata {
namespace {

void StoreLittleEndian(std::uint8_t* target, std::uint32_t value) {
    target[0] = static_cast<std::uint8_t>(value);
    target[1] = static_cast<std::uint8_t>(value >> 8);
    target[2] = static_cast<std::uint8_t>(value >> 16);
    target[3] = static_cast<std::uint8_t>(value >> 24);
}

} // namespace

std::vector<std::int16_t> ConvertPcmToStereo(const void* source, std::uint32_t frames,
                                             std::uint8_t channels, bool is_float, float gain) {
    if (source == nullptr || frames == 0 || (channels != 1 && channels != 2 && channels != 8)) {
        return {};
    }
    const float clamped_gain = std::clamp(gain, 0.0f, 1.0f);
    std::vector<std::int16_t> result(static_cast<std::size_t>(frames) * 2);
    const auto convert = [clamped_gain](float sample) {
        const float scaled = std::clamp(sample * clamped_gain, -1.0f, 1.0f);
        if (scaled <= -1.0f) {
            return std::int16_t{-32768};
        }
        return static_cast<std::int16_t>(std::lround(scaled * 32767.0f));
    };
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        if (is_float) {
            const auto* samples = static_cast<const float*>(source) + frame * channels;
            left = samples[0];
            right = channels == 1 ? left : samples[1];
        } else {
            const auto* samples = static_cast<const std::int16_t*>(source) + frame * channels;
            left = samples[0] / 32768.0f;
            right = channels == 1 ? left : samples[1] / 32768.0f;
        }
        result[frame * 2] = convert(left);
        result[frame * 2 + 1] = convert(right);
    }
    return result;
}

AudioTransport::AudioTransport(int fd) : fd_(fd) {}

AudioTransport::AudioTransport(AudioTransport&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

AudioTransport& AudioTransport::operator=(AudioTransport&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

AudioTransport::~AudioTransport() {
    Close();
}

std::optional<AudioTransport> AudioTransport::Connect(const std::filesystem::path& socket_path) {
#ifdef _WIN32
    (void)socket_path;
    return std::nullopt;
#else
    const auto native = socket_path.string();
    if (!socket_path.is_absolute() || native.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, native.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return AudioTransport(fd);
#endif
}

bool AudioTransport::Prepare(std::uint8_t channels, AudioSampleType type,
                             std::uint32_t sample_rate, std::uint32_t buffer_size) {
    std::array<std::uint8_t, 10> payload{channels, static_cast<std::uint8_t>(type)};
    StoreLittleEndian(payload.data() + 2, sample_rate);
    StoreLittleEndian(payload.data() + 6, buffer_size);
    return SendRequest(4, payload);
}

bool AudioTransport::Write(std::span<const std::uint8_t> pcm) {
    return !pcm.empty() && SendRequest(5, pcm);
}

bool AudioTransport::IsConnected() const {
    return fd_ >= 0;
}

bool AudioTransport::SendRequest(std::uint8_t code, std::span<const std::uint8_t> payload) {
    if (!IsConnected() || payload.size() > UINT32_MAX) {
        return false;
    }
    std::array<std::uint8_t, 5> header{code};
    StoreLittleEndian(header.data() + 1, static_cast<std::uint32_t>(payload.size()));
    const std::array<std::span<const std::uint8_t>, 2> parts{header, payload};
#ifdef _WIN32
    return false;
#else
    for (auto part : parts) {
        while (!part.empty()) {
            const ssize_t written = ::send(fd_, part.data(), part.size(), MSG_NOSIGNAL);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            part = part.subspan(static_cast<std::size_t>(written));
        }
    }
    return true;
#endif
}

void AudioTransport::Close() {
#ifndef _WIN32
    if (fd_ >= 0) {
        SendRequest(0, {});
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

} // namespace Platform::Bachata
