// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>

#include "common/boot_timer.h"

namespace Common {

namespace {
std::atomic<int64_t> g_boot_start_ns{0};
}

void MarkBootStart() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    g_boot_start_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
                          std::memory_order_relaxed);
}

s64 BootElapsedMs() {
    const auto start_ns = g_boot_start_ns.load(std::memory_order_relaxed);
    if (start_ns == 0) {
        return -1;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return (now_ns - start_ns) / 1'000'000;
}

} // namespace Common
