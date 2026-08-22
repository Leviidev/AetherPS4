// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/runtime_client.h"
#include "platform/bachata/controller_snapshot.h"
#include "platform/bachata/audio_transport.h"
#include "platform/bachata/ffmpeg_cpu_policy.h"

extern "C" {
#include <libavutil/cpu.h>
}

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace {

#ifndef _WIN32
std::string ReadLine(int fd) {
    std::string line;
    char c = 0;
    while (::recv(fd, &c, 1, 0) == 1) {
        line.push_back(c);
        if (c == '\n') {
            break;
        }
    }
    return line;
}
#endif

} // namespace

namespace Platform::Bachata {

TEST(BachataControllerSnapshot, ParsesCompleteFrame) {
    const auto snapshot = ParseControllerSnapshot(
        "BACHATA/1 INPUT seq=42 buttons=0x0000000000000401 lx=0 ly=128 rx=255 ry=64 "
        "l2=12 r2=240 touch=1 tx=1919 ty=1079\n");

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->sequence, 42u);
    EXPECT_EQ(snapshot->buttons, 0x401u);
    EXPECT_EQ(snapshot->left_x, 0);
    EXPECT_EQ(snapshot->left_y, 128);
    EXPECT_EQ(snapshot->right_x, 255);
    EXPECT_EQ(snapshot->right_y, 64);
    EXPECT_EQ(snapshot->left_trigger, 12);
    EXPECT_EQ(snapshot->right_trigger, 240);
    EXPECT_TRUE(snapshot->touch_down);
    EXPECT_EQ(snapshot->touch_x, 1919);
    EXPECT_EQ(snapshot->touch_y, 1079);
}

TEST(BachataControllerSnapshot, RejectsMalformedAndOutOfRangeFrames) {
    EXPECT_FALSE(ParseControllerSnapshot("BACHATA/1 INPUT seq=nope\n").has_value());
    EXPECT_FALSE(ParseControllerSnapshot(
                     "BACHATA/1 INPUT seq=1 buttons=0 lx=256 ly=128 rx=128 ry=128 l2=0 r2=0 "
                     "touch=0 tx=0 ty=0\n")
                     .has_value());
    EXPECT_FALSE(ParseControllerSnapshot(
                     "BACHATA/1 INPUT seq=1 buttons=0 lx=128 ly=128 rx=128 ry=128 l2=0 r2=0 "
                     "touch=2 tx=0 ty=0\n")
                     .has_value());
}

TEST(BachataControllerSnapshot, RejectsDuplicateUnknownAndMissingFields) {
    EXPECT_FALSE(ParseControllerSnapshot(
                     "BACHATA/1 INPUT seq=1 seq=2 buttons=0 lx=128 ly=128 rx=128 ry=128 l2=0 "
                     "r2=0 touch=0 tx=0 ty=0\n")
                     .has_value());
    EXPECT_FALSE(ParseControllerSnapshot(
                     "BACHATA/1 INPUT seq=1 buttons=0 lx=128 ly=128 rx=128 ry=128 l2=0 r2=0 "
                     "touch=0 tx=0 ty=0 extra=1\n")
                     .has_value());
    EXPECT_FALSE(ParseControllerSnapshot(
                     "BACHATA/1 INPUT seq=1 buttons=0 lx=128 ly=128 rx=128 ry=128 l2=0 r2=0 "
                     "touch=0 tx=0\n")
                     .has_value());
}

TEST(BachataRuntimeClient, ParsesVersionHandshake) {
    const auto frame = ParseRuntimeFrame("BACHATA/1 HELLO version=1\n");

    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->event, RuntimeEvent::Hello);
    EXPECT_EQ(frame->version, RuntimeProtocolVersion);
}

TEST(BachataRuntimeClient, ParsesLifecycleEvents) {
    const auto starting = ParseRuntimeFrame("BACHATA/1 EVENT Starting\n");
    const auto running = ParseRuntimeFrame("BACHATA/1 EVENT Running\n");
    const auto stopped = ParseRuntimeFrame("BACHATA/1 EVENT Stopped exit_code=7\n");

    ASSERT_TRUE(starting.has_value());
    ASSERT_TRUE(running.has_value());
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(starting->event, RuntimeEvent::Starting);
    EXPECT_EQ(running->event, RuntimeEvent::Running);
    EXPECT_EQ(stopped->event, RuntimeEvent::Stopped);
    EXPECT_EQ(stopped->exit_code, 7);
}

TEST(BachataRuntimeClient, RejectsMalformedFrames) {
    EXPECT_FALSE(ParseRuntimeFrame("BACHATA/2 HELLO version=2\n").has_value());
    EXPECT_FALSE(ParseRuntimeFrame("BACHATA/1 EVENT Stopped\n").has_value());
    EXPECT_FALSE(ParseRuntimeFrame("BACHATA/1 EVENT Unknown\n").has_value());
    EXPECT_FALSE(ParseRuntimeFrame("garbage\n").has_value());
}

TEST(BachataRuntimeClient, ValidatesSocketPathUnderRuntimeRoot) {
    EXPECT_TRUE(
        ValidateSocketPath("/data/user/0/app/runtime/bachata.sock", "/data/user/0/app/runtime"));
    EXPECT_FALSE(ValidateSocketPath("runtime/bachata.sock", "/data/user/0/app/runtime"));
    EXPECT_FALSE(ValidateSocketPath("/data/user/0/app/other.sock", "/data/user/0/app/runtime"));
}

TEST(BachataRuntimeClient, DisabledClientIsNoOp) {
    auto client = RuntimeClient::Disabled();

    EXPECT_FALSE(client.IsEnabled());
    EXPECT_TRUE(client.SendHello());
    EXPECT_TRUE(client.SendStarting());
    EXPECT_TRUE(client.SendRunning());
    EXPECT_TRUE(client.SendStopped(0));
    EXPECT_TRUE(client.SendError("CONTENT_INVALID"));
}

#ifndef _WIN32
TEST(BachataRuntimeClient, SendsLifecycleFramesOverSocketPair) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);

    RuntimeClient client(fds[0]);
    ASSERT_TRUE(client.SendHello());
    ASSERT_TRUE(client.SendStarting());
    ASSERT_TRUE(client.SendRunning());
    ASSERT_TRUE(client.SendStopped(3));

    EXPECT_EQ(ReadLine(fds[1]), "BACHATA/1 HELLO version=1\n");
    EXPECT_EQ(ReadLine(fds[1]), "BACHATA/1 EVENT Starting\n");
    EXPECT_EQ(ReadLine(fds[1]), "BACHATA/1 EVENT Running\n");
    EXPECT_EQ(ReadLine(fds[1]), "BACHATA/1 EVENT Stopped exit_code=3\n");
    ::close(fds[1]);
}

TEST(BachataRuntimeClient, ReportsDisconnectedPeer) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    ::close(fds[1]);

    RuntimeClient client(fds[0]);
    EXPECT_FALSE(client.SendStarting());
}

TEST(BachataRuntimeClient, ReadsOrderedInputAndReleasesOnDisconnect) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ControllerSnapshot> received;
    RuntimeClient client(fds[0]);
    ASSERT_TRUE(client.StartInputReader([&](const ControllerSnapshot& snapshot) {
        std::lock_guard lock(mutex);
        received.push_back(snapshot);
        changed.notify_all();
    }));

    const std::string valid =
        "BACHATA/1 INPUT seq=2 buttons=16384 lx=0 ly=128 rx=255 ry=64 l2=12 r2=240 "
        "touch=1 tx=100 ty=200\n";
    const std::string stale =
        "BACHATA/1 INPUT seq=1 buttons=32768 lx=128 ly=128 rx=128 ry=128 l2=0 r2=0 "
        "touch=0 tx=0 ty=0\n";
    const std::string malformed = "BACHATA/1 INPUT invalid\n";
    ASSERT_EQ(::send(fds[1], valid.data(), valid.size(), 0), valid.size());
    ASSERT_EQ(::send(fds[1], stale.data(), stale.size(), 0), stale.size());
    ASSERT_EQ(::send(fds[1], malformed.data(), malformed.size(), 0), malformed.size());
    ::close(fds[1]);

    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return received.size() == 2; }));
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0].sequence, 2u);
    EXPECT_EQ(received[0].buttons, 16384u);
    EXPECT_EQ(received[1].sequence, 3u);
    EXPECT_EQ(received[1].buttons, 0u);
    EXPECT_EQ(received[1].left_x, 128);
    EXPECT_EQ(received[1].right_trigger, 0);
    EXPECT_FALSE(received[1].touch_down);
}

TEST(BachataAudioTransport, WritesWinlatorProtocolFrames) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    {
        AudioTransport transport(fds[0]);
        ASSERT_TRUE(transport.Prepare(2, AudioSampleType::S16LittleEndian, 48000, 4096));
        const std::array<std::uint8_t, 4> pcm{1, 2, 3, 4};
        ASSERT_TRUE(transport.Write(pcm));
    }

    std::array<std::uint8_t, 29> bytes{};
    ASSERT_EQ(::recv(fds[1], bytes.data(), bytes.size(), MSG_WAITALL), bytes.size());
    const std::array<std::uint8_t, 29> expected{
        4, 10, 0, 0, 0, 2, 1, 0x80, 0xbb, 0, 0, 0, 0x10, 0, 0,
        5, 4, 0, 0, 0, 1, 2, 3, 4,
        0, 0, 0, 0, 0,
    };
    EXPECT_EQ(bytes, expected);
    ::close(fds[1]);
}

TEST(BachataAudioTransport, ConvertsGuestPcmToStereoS16) {
    const std::array<std::int16_t, 2> mono{-16384, 16384};
    EXPECT_EQ(ConvertPcmToStereo(mono.data(), 2, 1, false, 1.0f),
              (std::vector<std::int16_t>{-16384, -16384, 16384, 16384}));

    const std::array<float, 4> stereo{-2.0f, -0.5f, 0.5f, 2.0f};
    EXPECT_EQ(ConvertPcmToStereo(stereo.data(), 2, 2, true, 1.0f),
              (std::vector<std::int16_t>{-32768, -16384, 16384, 32767}));

    const std::array<std::int16_t, 8> surround{100, 200, 300, 400, 500, 600, 700, 800};
    EXPECT_EQ(ConvertPcmToStereo(surround.data(), 1, 8, false, 0.5f),
              (std::vector<std::int16_t>{50, 100}));
}

TEST(BachataFfmpegCpuPolicy, DisablesCpuSpecificOptimizations) {
    av_force_cpu_flags(-1);
    const int detected_flags = av_get_cpu_flags();
    ASSERT_NE(detected_flags, 0);

    {
        ScopedBox64CompatibleFfmpegCpuPolicy policy;
        EXPECT_EQ(av_get_cpu_flags(), 0);
    }

    EXPECT_EQ(av_get_cpu_flags(), detected_flags);
}
#endif

} // namespace Platform::Bachata
