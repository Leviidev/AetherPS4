// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/ios/touch_controls_layer.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <cmath>

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "common/singleton.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"

namespace Platform::iOS {

namespace {
using Libraries::Pad::OrbisPadButtonDataOffset;

// A dedicated slot (the last of GameControllers' 5), never the same one a real physical
// controller would occupy first -- see TouchControlsLayer's own header comment for why
// ApplyRemoteState() is the right call to make here at all.
constexpr size_t kTouchControllerSlot = 4;

bool WithinCircle(float px, float py, float cx, float cy, float radius) {
    const float dx = px - cx;
    const float dy = py - cy;
    return (dx * dx + dy * dy) <= radius * radius;
}

bool WithinRect(float px, float py, float cx, float cy, float halfW, float halfH) {
    return px >= cx - halfW && px <= cx + halfW && py >= cy - halfH && py <= cy + halfH;
}

// Clamps (x, y) to within `radius` of (cx, cy), returning the clamped offset from center --
// shared by both the visual thumb position (Draw) and the actual axis value (PushControllerState)
// so they never disagree about where "fully deflected" is.
void ClampStickOffset(float cx, float cy, float x, float y, float radius, float& outDx,
                      float& outDy) {
    float dx = x - cx;
    float dy = y - cy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > radius && dist > 0.0f) {
        const float scale = radius / dist;
        dx *= scale;
        dy *= scale;
    }
    outDx = dx;
    outDy = dy;
}
} // namespace

TouchControlsLayer::Layout TouchControlsLayer::ComputeLayout() const {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = viewport->Size.x;
    const float h = viewport->Size.y;
    const float ox = viewport->Pos.x;
    const float oy = viewport->Pos.y;
    // `u` is 1% of the screen height -- landscape's smaller dimension -- used as the base
    // unit for every radius/offset below so the whole layout scales uniformly regardless of
    // actual device resolution, matching mobile_overlay.cpp's own established
    // "GetMainViewport(), not raw io.DisplaySize" pattern for this iOS backend.
    const float u = h * 0.01f;

    Layout layout;
    layout.stickRadius = u * 17.0f;
    layout.leftStickX = ox + u * 20.0f;
    layout.leftStickY = oy + h - u * 22.0f;

    layout.dpadRadius = u * 13.0f;
    layout.dpadX = ox + u * 18.0f;
    layout.dpadY = oy + u * 36.0f;

    layout.rightStickX = ox + w - u * 36.0f;
    layout.rightStickY = oy + h - u * 22.0f;

    layout.faceButtonRadius = u * 8.5f;
    const float faceCenterX = ox + w - u * 16.0f;
    const float faceCenterY = oy + u * 42.0f;
    const float faceSpread = u * 12.0f;
    layout.triangleX = faceCenterX;
    layout.triangleY = faceCenterY - faceSpread;
    layout.crossX = faceCenterX;
    layout.crossY = faceCenterY + faceSpread;
    layout.squareX = faceCenterX - faceSpread;
    layout.squareY = faceCenterY;
    layout.circleX = faceCenterX + faceSpread;
    layout.circleY = faceCenterY;

    layout.shoulderW = u * 16.0f;
    layout.shoulderH = u * 8.0f;
    layout.l1X = ox + u * 10.0f;
    layout.l1Y = oy + u * 12.0f;
    layout.l2X = ox + u * 10.0f;
    layout.l2Y = oy + u * 2.0f;
    layout.r1X = ox + w - u * 24.0f;
    layout.r1Y = oy + u * 12.0f;
    layout.r2X = ox + w - u * 24.0f;
    layout.r2Y = oy + u * 2.0f;

    layout.smallButtonRadius = u * 4.5f;
    layout.optionsX = ox + w * 0.58f;
    layout.optionsY = oy + u * 6.0f;
    layout.touchPadX = ox + w * 0.42f;
    layout.touchPadY = oy + u * 6.0f;

    return layout;
}

std::optional<TouchControlsLayer::ControlId> TouchControlsLayer::HitTest(const Layout& layout,
                                                                          float normX,
                                                                          float normY) const {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float x = viewport->Pos.x + normX * viewport->Size.x;
    const float y = viewport->Pos.y + normY * viewport->Size.y;

    if (WithinCircle(x, y, layout.leftStickX, layout.leftStickY, layout.stickRadius)) {
        return ControlId::LeftStick;
    }
    if (WithinCircle(x, y, layout.rightStickX, layout.rightStickY, layout.stickRadius)) {
        return ControlId::RightStick;
    }
    if (WithinCircle(x, y, layout.triangleX, layout.triangleY, layout.faceButtonRadius)) {
        return ControlId::Triangle;
    }
    if (WithinCircle(x, y, layout.circleX, layout.circleY, layout.faceButtonRadius)) {
        return ControlId::Circle;
    }
    if (WithinCircle(x, y, layout.crossX, layout.crossY, layout.faceButtonRadius)) {
        return ControlId::Cross;
    }
    if (WithinCircle(x, y, layout.squareX, layout.squareY, layout.faceButtonRadius)) {
        return ControlId::Square;
    }
    if (WithinRect(x, y, layout.l1X, layout.l1Y, layout.shoulderW * 0.5f, layout.shoulderH * 0.5f)) {
        return ControlId::L1;
    }
    if (WithinRect(x, y, layout.l2X, layout.l2Y, layout.shoulderW * 0.5f, layout.shoulderH * 0.5f)) {
        return ControlId::L2;
    }
    if (WithinRect(x, y, layout.r1X, layout.r1Y, layout.shoulderW * 0.5f, layout.shoulderH * 0.5f)) {
        return ControlId::R1;
    }
    if (WithinRect(x, y, layout.r2X, layout.r2Y, layout.shoulderW * 0.5f, layout.shoulderH * 0.5f)) {
        return ControlId::R2;
    }
    if (WithinCircle(x, y, layout.optionsX, layout.optionsY, layout.smallButtonRadius)) {
        return ControlId::Options;
    }
    if (WithinCircle(x, y, layout.touchPadX, layout.touchPadY, layout.smallButtonRadius)) {
        return ControlId::TouchPad;
    }
    if (WithinCircle(x, y, layout.dpadX, layout.dpadY, layout.dpadRadius)) {
        // Angle-based 8-way direction within the d-pad's circular zone, rather than 4
        // separate quadrant rectangles -- lets a touch near a corner register as a diagonal
        // (two bits at once) the same way a real d-pad's corner does.
        const float dx = x - layout.dpadX;
        const float dy = y - layout.dpadY;
        // Screen Y grows downward; negate dy so "up" maps to the conventional +90 degrees.
        const float degrees = std::atan2(-dy, dx) * (180.0f / 3.14159265f);
        const float normalized = std::fmod(degrees + 360.0f, 360.0f);
        if (normalized >= 337.5f || normalized < 22.5f) return ControlId::DPadRight;
        if (normalized < 67.5f) return ControlId::DPadUpRight;
        if (normalized < 112.5f) return ControlId::DPadUp;
        if (normalized < 157.5f) return ControlId::DPadUpLeft;
        if (normalized < 202.5f) return ControlId::DPadLeft;
        if (normalized < 247.5f) return ControlId::DPadDownLeft;
        if (normalized < 292.5f) return ControlId::DPadDown;
        return ControlId::DPadDownRight;
    }
    return std::nullopt;
}

bool TouchControlsLayer::OnFingerEvent(const SDL_Event& event) {
    if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_UP &&
        event.type != SDL_EVENT_FINGER_MOTION && event.type != SDL_EVENT_FINGER_CANCELED) {
        return false;
    }
    const auto& finger = event.tfinger;

    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        const auto it = active_touches.find(finger.fingerID);
        if (it == active_touches.end()) {
            return false;
        }
        active_touches.erase(it);
        return true;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = viewport->Pos.x + finger.x * viewport->Size.x;
    const float py = viewport->Pos.y + finger.y * viewport->Size.y;

    if (event.type == SDL_EVENT_FINGER_MOTION) {
        const auto it = active_touches.find(finger.fingerID);
        if (it == active_touches.end()) {
            return false;
        }
        it->second.currentX = px;
        it->second.currentY = py;
        return true;
    }

    // SDL_EVENT_FINGER_DOWN
    const auto control = HitTest(last_layout, finger.x, finger.y);
    if (!control.has_value()) {
        return false;
    }
    ActiveTouch touch;
    touch.control = *control;
    touch.currentX = px;
    touch.currentY = py;
    if (*control == ControlId::LeftStick) {
        touch.centerX = last_layout.leftStickX;
        touch.centerY = last_layout.leftStickY;
    } else if (*control == ControlId::RightStick) {
        touch.centerX = last_layout.rightStickX;
        touch.centerY = last_layout.rightStickY;
    }
    active_touches[finger.fingerID] = touch;
    return true;
}

void TouchControlsLayer::PushControllerState(const Layout& layout) const {
    OrbisPadButtonDataOffset buttons = OrbisPadButtonDataOffset::None;
    std::array<int, 6> axes = {128, 128, 128, 128, 0, 0};

    for (const auto& [finger_id, touch] : active_touches) {
        switch (touch.control) {
        case ControlId::DPadUp:
            buttons |= OrbisPadButtonDataOffset::Up;
            break;
        case ControlId::DPadDown:
            buttons |= OrbisPadButtonDataOffset::Down;
            break;
        case ControlId::DPadLeft:
            buttons |= OrbisPadButtonDataOffset::Left;
            break;
        case ControlId::DPadRight:
            buttons |= OrbisPadButtonDataOffset::Right;
            break;
        case ControlId::DPadUpRight:
            buttons |= OrbisPadButtonDataOffset::Up | OrbisPadButtonDataOffset::Right;
            break;
        case ControlId::DPadDownRight:
            buttons |= OrbisPadButtonDataOffset::Down | OrbisPadButtonDataOffset::Right;
            break;
        case ControlId::DPadDownLeft:
            buttons |= OrbisPadButtonDataOffset::Down | OrbisPadButtonDataOffset::Left;
            break;
        case ControlId::DPadUpLeft:
            buttons |= OrbisPadButtonDataOffset::Up | OrbisPadButtonDataOffset::Left;
            break;
        case ControlId::Triangle:
            buttons |= OrbisPadButtonDataOffset::Triangle;
            break;
        case ControlId::Circle:
            buttons |= OrbisPadButtonDataOffset::Circle;
            break;
        case ControlId::Cross:
            buttons |= OrbisPadButtonDataOffset::Cross;
            break;
        case ControlId::Square:
            buttons |= OrbisPadButtonDataOffset::Square;
            break;
        case ControlId::L1:
            buttons |= OrbisPadButtonDataOffset::L1;
            break;
        case ControlId::R1:
            buttons |= OrbisPadButtonDataOffset::R1;
            break;
        case ControlId::L2:
            // Reported as both the button bit and a fully-deflected analog trigger --
            // real hardware reports L2/R2 both ways simultaneously, and touch has no
            // meaningful partial-pressure signal to map to anything in between.
            buttons |= OrbisPadButtonDataOffset::L2;
            axes[std::to_underlying(Input::Axis::TriggerLeft)] = 255;
            break;
        case ControlId::R2:
            buttons |= OrbisPadButtonDataOffset::R2;
            axes[std::to_underlying(Input::Axis::TriggerRight)] = 255;
            break;
        case ControlId::Options:
            buttons |= OrbisPadButtonDataOffset::Options;
            break;
        case ControlId::TouchPad:
            buttons |= OrbisPadButtonDataOffset::TouchPad;
            break;
        case ControlId::LeftStick: {
            float dx, dy;
            ClampStickOffset(touch.centerX, touch.centerY, touch.currentX, touch.currentY,
                             layout.stickRadius, dx, dy);
            axes[std::to_underlying(Input::Axis::LeftX)] =
                Input::GetAxis(-static_cast<int>(layout.stickRadius),
                               static_cast<int>(layout.stickRadius), static_cast<int>(dx));
            axes[std::to_underlying(Input::Axis::LeftY)] =
                Input::GetAxis(-static_cast<int>(layout.stickRadius),
                               static_cast<int>(layout.stickRadius), static_cast<int>(dy));
            break;
        }
        case ControlId::RightStick: {
            float dx, dy;
            ClampStickOffset(touch.centerX, touch.centerY, touch.currentX, touch.currentY,
                             layout.stickRadius, dx, dy);
            axes[std::to_underlying(Input::Axis::RightX)] =
                Input::GetAxis(-static_cast<int>(layout.stickRadius),
                               static_cast<int>(layout.stickRadius), static_cast<int>(dx));
            axes[std::to_underlying(Input::Axis::RightY)] =
                Input::GetAxis(-static_cast<int>(layout.stickRadius),
                               static_cast<int>(layout.stickRadius), static_cast<int>(dy));
            break;
        }
        }
    }

    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    (*controllers)[kTouchControllerSlot]->ApplyRemoteState(buttons, axes, false, 0.0f, 0.0f);
}

void TouchControlsLayer::Draw() {
    const Layout layout = ComputeLayout();
    last_layout = layout;

    PushControllerState(layout);

    const auto isPressed = [this](ControlId id) {
        for (const auto& [finger_id, touch] : active_touches) {
            if (touch.control == id) return true;
        }
        return false;
    };
    const auto isDpadPressed = [this](ControlId primary, ControlId diagA, ControlId diagB) {
        for (const auto& [finger_id, touch] : active_touches) {
            if (touch.control == primary || touch.control == diagA || touch.control == diagB) {
                return true;
            }
        }
        return false;
    };

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    constexpr ImU32 kBase = IM_COL32(255, 255, 255, 50);
    constexpr ImU32 kOutline = IM_COL32(255, 255, 255, 100);
    constexpr ImU32 kPressed = IM_COL32(255, 255, 255, 150);
    constexpr ImU32 kShapeOutline = IM_COL32(20, 20, 20, 180);

    // Left stick base + thumb.
    draw->AddCircleFilled(ImVec2(layout.leftStickX, layout.leftStickY), layout.stickRadius, kBase,
                          32);
    draw->AddCircle(ImVec2(layout.leftStickX, layout.leftStickY), layout.stickRadius, kOutline, 32,
                    2.0f);
    // Right stick base + thumb.
    draw->AddCircleFilled(ImVec2(layout.rightStickX, layout.rightStickY), layout.stickRadius,
                          kBase, 32);
    draw->AddCircle(ImVec2(layout.rightStickX, layout.rightStickY), layout.stickRadius, kOutline,
                    32, 2.0f);
    for (const auto& [finger_id, touch] : active_touches) {
        if (touch.control != ControlId::LeftStick && touch.control != ControlId::RightStick) {
            continue;
        }
        float dx, dy;
        ClampStickOffset(touch.centerX, touch.centerY, touch.currentX, touch.currentY,
                         layout.stickRadius, dx, dy);
        draw->AddCircleFilled(ImVec2(touch.centerX + dx, touch.centerY + dy),
                              layout.stickRadius * 0.45f, kPressed, 24);
    }
    // Default (un-pressed) thumb position for whichever stick has no active touch.
    if (!isPressed(ControlId::LeftStick)) {
        draw->AddCircleFilled(ImVec2(layout.leftStickX, layout.leftStickY),
                              layout.stickRadius * 0.45f, kBase, 24);
    }
    if (!isPressed(ControlId::RightStick)) {
        draw->AddCircleFilled(ImVec2(layout.rightStickX, layout.rightStickY),
                              layout.stickRadius * 0.45f, kBase, 24);
    }

    // D-pad: a plus-shaped outline with each arm brightened when its direction (including
    // the two adjacent diagonals) is currently pressed.
    {
        const float r = layout.dpadRadius;
        const float armW = r * 0.55f;
        const ImVec2 c(layout.dpadX, layout.dpadY);
        draw->AddCircleFilled(c, r, kBase, 32);
        const ImU32 up = isDpadPressed(ControlId::DPadUp, ControlId::DPadUpLeft,
                                       ControlId::DPadUpRight)
                            ? kPressed
                            : kOutline;
        const ImU32 down = isDpadPressed(ControlId::DPadDown, ControlId::DPadDownLeft,
                                         ControlId::DPadDownRight)
                              ? kPressed
                              : kOutline;
        const ImU32 left = isDpadPressed(ControlId::DPadLeft, ControlId::DPadUpLeft,
                                         ControlId::DPadDownLeft)
                              ? kPressed
                              : kOutline;
        const ImU32 right = isDpadPressed(ControlId::DPadRight, ControlId::DPadUpRight,
                                          ControlId::DPadDownRight)
                               ? kPressed
                               : kOutline;
        draw->AddTriangleFilled(ImVec2(c.x - armW * 0.5f, c.y - armW * 0.5f),
                                ImVec2(c.x + armW * 0.5f, c.y - armW * 0.5f),
                                ImVec2(c.x, c.y - r), up);
        draw->AddTriangleFilled(ImVec2(c.x - armW * 0.5f, c.y + armW * 0.5f),
                                ImVec2(c.x + armW * 0.5f, c.y + armW * 0.5f),
                                ImVec2(c.x, c.y + r), down);
        draw->AddTriangleFilled(ImVec2(c.x - armW * 0.5f, c.y - armW * 0.5f),
                                ImVec2(c.x - armW * 0.5f, c.y + armW * 0.5f),
                                ImVec2(c.x - r, c.y), left);
        draw->AddTriangleFilled(ImVec2(c.x + armW * 0.5f, c.y - armW * 0.5f),
                                ImVec2(c.x + armW * 0.5f, c.y + armW * 0.5f),
                                ImVec2(c.x + r, c.y), right);
        draw->AddCircle(c, r, kOutline, 32, 2.0f);
    }

    // Face buttons: actual PlayStation shapes, not text glyphs (no guarantee the loaded
    // ImGui font has them) -- triangle outline, circle outline, X from two crossed lines,
    // square outline, each on its own translucent round button.
    const auto drawFaceButton = [&](float x, float y, ControlId id, auto&& drawGlyph) {
        const ImU32 fill = isPressed(id) ? kPressed : kBase;
        draw->AddCircleFilled(ImVec2(x, y), layout.faceButtonRadius, fill, 24);
        draw->AddCircle(ImVec2(x, y), layout.faceButtonRadius, kOutline, 24, 1.5f);
        drawGlyph(x, y, layout.faceButtonRadius * 0.5f);
    };
    drawFaceButton(layout.triangleX, layout.triangleY, ControlId::Triangle,
                   [&](float x, float y, float s) {
                       draw->AddTriangle(ImVec2(x, y - s), ImVec2(x + s, y + s * 0.7f),
                                        ImVec2(x - s, y + s * 0.7f), kShapeOutline, 2.5f);
                   });
    drawFaceButton(layout.circleX, layout.circleY, ControlId::Circle,
                   [&](float x, float y, float s) {
                       draw->AddCircle(ImVec2(x, y), s, kShapeOutline, 20, 2.5f);
                   });
    drawFaceButton(layout.crossX, layout.crossY, ControlId::Cross,
                   [&](float x, float y, float s) {
                       draw->AddLine(ImVec2(x - s, y - s), ImVec2(x + s, y + s), kShapeOutline,
                                     2.5f);
                       draw->AddLine(ImVec2(x - s, y + s), ImVec2(x + s, y - s), kShapeOutline,
                                     2.5f);
                   });
    drawFaceButton(layout.squareX, layout.squareY, ControlId::Square,
                   [&](float x, float y, float s) {
                       draw->AddRect(ImVec2(x - s, y - s), ImVec2(x + s, y + s), kShapeOutline,
                                     0.0f, 0, 2.5f);
                   });

    // Shoulder buttons: simple rounded rectangles.
    const auto drawShoulder = [&](float x, float y, ControlId id, const char* label) {
        const ImU32 fill = isPressed(id) ? kPressed : kBase;
        const ImVec2 min(x - layout.shoulderW * 0.5f, y - layout.shoulderH * 0.5f);
        const ImVec2 max(x + layout.shoulderW * 0.5f, y + layout.shoulderH * 0.5f);
        draw->AddRectFilled(min, max, fill, 6.0f);
        draw->AddRect(min, max, kOutline, 6.0f, 0, 1.5f);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(x - textSize.x * 0.5f, y - textSize.y * 0.5f), kShapeOutline, label);
    };
    drawShoulder(layout.l1X, layout.l1Y, ControlId::L1, "L1");
    drawShoulder(layout.l2X, layout.l2Y, ControlId::L2, "L2");
    drawShoulder(layout.r1X, layout.r1Y, ControlId::R1, "R1");
    drawShoulder(layout.r2X, layout.r2Y, ControlId::R2, "R2");

    // Options / TouchPad: small round buttons.
    const auto drawSmallButton = [&](float x, float y, ControlId id, const char* label) {
        const ImU32 fill = isPressed(id) ? kPressed : kBase;
        draw->AddCircleFilled(ImVec2(x, y), layout.smallButtonRadius, fill, 20);
        draw->AddCircle(ImVec2(x, y), layout.smallButtonRadius, kOutline, 20, 1.5f);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(x - textSize.x * 0.5f, y - textSize.y * 0.5f), kShapeOutline, label);
    };
    drawSmallButton(layout.optionsX, layout.optionsY, ControlId::Options, "OPT");
    drawSmallButton(layout.touchPadX, layout.touchPadY, ControlId::TouchPad, "TP");
}

} // namespace Platform::iOS

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
