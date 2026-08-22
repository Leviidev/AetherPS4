// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <string>

#include "imgui/imgui_layer.h"

namespace Platform::iOS {

// Replaces the old second-UIWindow SwiftUI loading overlay (GameOverlayWindow.swift /
// GameOverlayView.swift), which turned out to be unable to reliably update once
// shadps4_run() hands the screen/run loop over to SDL: a console-toggle button tried
// there was confirmed on-device to wedge the UI (a tap registered, but the resulting
// SwiftUI state change never finished committing), and this session confirmed even a
// purely passive dismiss driven from a background-thread callback never visibly
// applied either -- nothing hosted in that second UIWindow reliably repaints once
// shadps4_run() takes over, not just the interactive parts.
//
// This renders inside the same Vulkan/ImGui frame loop that already draws the FPS
// counter (Core::Devtools::Layer, see layer.cpp's DrawSimple/"Video Info" window for
// the identical ImGuiWindowFlags_NoDecoration|AlwaysAutoResize|NoDocking pattern this
// borrows), which keeps updating every single frame regardless of what SDL is doing on
// the main thread, since it's driven by the render loop itself rather than iOS's UIKit
// run loop.
class MobileOverlayLayer final : public ImGui::Layer {
public:
    void Draw() override;

private:
    void DrawToggleButton();
    void DrawPanel();
    void RefreshConsoleTail();

    bool show_panel = true;
    std::string console_tail;
    double last_refresh_time = 0.0;
};

} // namespace Platform::iOS

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
