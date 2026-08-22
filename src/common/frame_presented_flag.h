// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

// Cross-cutting signal for "has the emulator successfully presented at least one real
// frame yet" -- set by the Vulkan presenter (vk_presenter.cpp) the first time
// swapchain.Present() succeeds, read by the iOS host app (shadps4_ios_api.cpp) so its
// Swift-side boot loading overlay knows when to dismiss itself in favor of the actual
// game video, rather than guessing with a fixed timeout.
namespace Common {

inline std::atomic<bool>& FramePresentedFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

// Fired exactly once, synchronously, from whichever thread first flips FramePresentedFlag
// to true (the Vulkan presenter's own render thread -- see vk_presenter.cpp) -- pushed
// rather than polled, so the host app finds out the instant it happens instead of up to
// one polling-interval late. Registered via shadps4_register_first_frame_callback
// (shadps4_ios_api.h) before shadps4_run() is called.
inline std::atomic<void (*)()>& FramePresentedCallback() {
  static std::atomic<void (*)()> callback{nullptr};
  return callback;
}

} // namespace Common
