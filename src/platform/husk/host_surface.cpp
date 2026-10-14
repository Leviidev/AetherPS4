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

bool HasHostSurface() {
    std::scoped_lock lock{g_mutex};
    return g_surface.metal_layer != nullptr;
}

HostSurface GetHostSurface() {
    std::scoped_lock lock{g_mutex};
    return g_surface;
}

} // namespace Frontend
