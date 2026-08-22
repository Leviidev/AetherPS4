// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "platform/bachata/audio_transport.h"

namespace Libraries::AudioOut {
namespace {

class BachataPortBackend final : public PortBackend {
public:
    explicit BachataPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          channels(port.format_info.num_channels), is_float(port.format_info.is_float) {
        const char* socket_path = std::getenv("BACHATA_ALSA_SOCKET");
        if (socket_path == nullptr || socket_path[0] != '/') {
            LOG_ERROR(Lib_AudioOut, "BACHATA_ALSA_SOCKET is unavailable");
            return;
        }
        transport = Platform::Bachata::AudioTransport::Connect(socket_path);
        const std::uint32_t output_size = buffer_frames * 2 * sizeof(std::int16_t);
        if (!transport || !transport->Prepare(2, Platform::Bachata::AudioSampleType::S16LittleEndian,
                                              sample_rate, output_size)) {
            transport.reset();
            LOG_ERROR(Lib_AudioOut, "Failed to connect Bachata audio transport");
            return;
        }
        LOG_INFO(Lib_AudioOut, "Opened Bachata audio transport ({} Hz, {} input channels)",
                 sample_rate, channels);
    }

    void Output(void* source) override {
        if (!transport || source == nullptr) {
            return;
        }
        const float global_gain = EmulatorSettings.GetVolumeSlider() * 0.01f;
        const auto pcm = Platform::Bachata::ConvertPcmToStereo(
            source, buffer_frames, channels, is_float,
            std::clamp(channel_gain.load(std::memory_order_relaxed) * global_gain, 0.0f, 1.0f));
        const auto bytes = std::as_bytes(std::span{pcm});
        if (!transport->Write({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()})) {
            LOG_ERROR(Lib_AudioOut, "Bachata audio transport disconnected");
            transport.reset();
        }
    }

    void SetVolume(const std::array<int, 8>& volumes) override {
        int maximum = 0;
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            maximum = std::max(maximum, std::abs(volumes[channel]));
        }
        channel_gain.store(std::clamp(maximum / 32768.0f, 0.0f, 1.0f),
                           std::memory_order_relaxed);
    }

private:
    std::optional<Platform::Bachata::AudioTransport> transport;
    std::uint32_t buffer_frames;
    std::uint32_t sample_rate;
    std::uint8_t channels;
    bool is_float;
    std::atomic<float> channel_gain{1.0f};
};

} // namespace

std::unique_ptr<PortBackend> BachataAudioOut::Open(PortOut& port) {
    return std::make_unique<BachataPortBackend>(port);
}

} // namespace Libraries::AudioOut
