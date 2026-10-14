// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Host surface: the embedding app (Husk) owns the window and hands the core a CAMetalLayer to
// render into. When one is set before the window is created, WindowSDL creates no SDL window and
// starts no SDL video: SDL runs audio and events only, the Vulkan surface is made on the layer,
// and ImGui takes its display size from here. Unset (the default), nothing changes.

namespace Frontend {

struct HostSurface {
    void* metal_layer = nullptr; // CAMetalLayer*
    int width_px = 0;
    int height_px = 0;
    float scale = 1.0f; // pixels per point
};

void SetHostSurface(const HostSurface& surface);
// Records a new drawable size; the presenter picks it up on its next resize check.
void ResizeHostSurface(int width_px, int height_px);
bool HasHostSurface();
HostSurface GetHostSurface();

// Set by libhuskps4 before the core starts: the core runs inside Husk, so it shows no dialogs of
// its own (no SDL message boxes; first-run migrations take their "leave it" answer).
void SetHuskHosted(bool hosted);
bool IsHuskHosted();

} // namespace Frontend
