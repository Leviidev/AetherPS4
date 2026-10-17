// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/husk/host_surface.h"

#include <atomic>
#include <mutex>

namespace Frontend {

namespace {
std::mutex g_mutex;
HostSurface g_surface;
} // namespace

void SetHostSurface(const HostSurface& surface) {
    std::scoped_lock lock{g_mutex};
    g_surface = surface;
}

void ResizeHostSurface(int width_px, int height_px) {
    std::scoped_lock lock{g_mutex};
    g_surface.width_px = width_px;
    g_surface.height_px = height_px;
}

namespace {
std::atomic<bool> g_husk_hosted{false};
} // namespace

void SetHuskHosted(bool hosted) {
    g_husk_hosted.store(hosted, std::memory_order_release);
}

bool IsHuskHosted() {
    return g_husk_hosted.load(std::memory_order_acquire);
}

namespace {
std::atomic<HostTextRequester> g_text_requester{nullptr};
} // namespace

void SetHostTextRequester(HostTextRequester requester) {
    g_text_requester.store(requester, std::memory_order_release);
}

bool RequestHostText(const char* title, const char* initial, unsigned max_chars, HostTextDone done,
                     void* ctx) {
    const auto requester = g_text_requester.load(std::memory_order_acquire);
    return requester != nullptr && requester(title, initial, max_chars, done, ctx) == 0;
}

bool HasHostSurface() {
    std::scoped_lock lock{g_mutex};
    return g_surface.metal_layer != nullptr;
}

HostSurface GetHostSurface() {
    std::scoped_lock lock{g_mutex};
    return g_surface;
}

} // namespace Frontend
