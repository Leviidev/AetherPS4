// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <optional>
#include <unordered_map>

#include "common/types.h"
#include "imgui/imgui_layer.h"

union SDL_Event;

namespace Platform::iOS {

// Fixed-layout, full PS4 controller touch overlay -- a from-scratch replacement for a
// physical controller, not a SwiftUI thing: touch input needs to be read and turned into
// controller state on every frame while the game is actually running, which is exactly the
// render-thread-driven work this whole class of overlay (see MobileOverlayLayer, its
// sibling in this same directory) needs, for the same reason SwiftUI can't do it -- see
// GameLoadingCoverView.swift's own header comment in the Swift sources for that history.
// Draws via ImGui's low-level ImDrawList directly (AddCircleFilled/AddRectFilled/AddLine),
// not ImGui's window/widget system -- nothing here looks or behaves like a generic ImGui
// panel; every shape is purpose-drawn PlayStation controller iconography.
//
// Reports its state through the same Input::GameControllers::ApplyRemoteState() path an
// external/network remote controller uses (see main.cpp's Bachata runtime input reader for
// the other real caller of that same function) -- occupying its own dedicated, high-index
// controller slot so it never collides with a real physical controller's slot.
class TouchControlsLayer final : public ImGui::Layer {
public:
    void Draw() override;

    // Called from WindowSDL::WaitEvent() (sdl_window.cpp), before SDL events reach ImGui's
    // own SDL backend -- that backend collapses every touch to a single emulated mouse
    // pointer (see imgui_impl_sdl3.cpp's own comment on that), which can't represent
    // holding a stick down with one thumb while pressing a face button with another at the
    // same time. Returns true if this event was consumed (the touch started inside a
    // control zone) and should not be processed any further by anything else.
    bool OnFingerEvent(const SDL_Event& event);

private:
    enum class ControlId {
        DPadUp,
        DPadUpRight,
        DPadRight,
        DPadDownRight,
        DPadDown,
        DPadDownLeft,
        DPadLeft,
        DPadUpLeft,
        Triangle,
        Circle,
        Cross,
        Square,
        L1,
        R1,
        L2,
        R2,
        Options,
        TouchPad,
        LeftStick,
        RightStick,
    };

    struct ActiveTouch {
        ControlId control;
        // Only meaningful for LeftStick/RightStick: the stick's own center, captured once
        // at touch-down so a stick's base doesn't visually or functionally jump to wherever
        // the finger first landed within its zone -- this is a fixed-position stick, not a
        // floating/relative one.
        float centerX{};
        float centerY{};
        float currentX{};
        float currentY{};
    };

    // Layout is computed fresh each frame from the current viewport size (see Draw()) --
    // these are cached from the most recent Draw() call purely so OnFingerEvent(), which
    // can run on a different call than Draw() (SDL events are pumped independently of the
    // render loop), hit-tests against up-to-date geometry rather than stale values from
    // whenever the layout was first computed. Landscape-only, matching the rest of this
    // port (see EmulatorProcess.lockToLandscape's own comment for why).
    struct Layout {
        float leftStickX{}, leftStickY{}, stickRadius{};
        float rightStickX{}, rightStickY{};
        float dpadX{}, dpadY{}, dpadRadius{};
        float faceButtonRadius{};
        float triangleX{}, triangleY{};
        float circleX{}, circleY{};
        float crossX{}, crossY{};
        float squareX{}, squareY{};
        float l1X{}, l1Y{}, l2X{}, l2Y{}, shoulderW{}, shoulderH{};
        float r1X{}, r1Y{}, r2X{}, r2Y{};
        float optionsX{}, optionsY{}, touchPadX{}, touchPadY{}, smallButtonRadius{};
    };

    Layout ComputeLayout() const;
    std::optional<ControlId> HitTest(const Layout& layout, float normX, float normY) const;
    void PushControllerState(const Layout& layout) const;

    Layout last_layout{};
    std::unordered_map<u64, ActiveTouch> active_touches;
};

} // namespace Platform::iOS

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
