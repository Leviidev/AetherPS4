// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/ios/mobile_overlay.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <algorithm>
#include <fstream>
#include <sstream>

#include <imgui.h>
#include <imgui_internal.h>

#include "common/frame_presented_flag.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#endif

namespace Platform::iOS {

namespace {
constexpr double kConsoleRefreshIntervalSeconds = 0.5;

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
// Polls HandleGuestSignal's atomic checkpoint (see fex_guest_engine.h) once per frame
// from this thread -- never the one that might be stuck inside the signal handler --
// and logs any change via the normal (non-signal-context) logger. Every attempt to log
// directly from inside HandleGuestSignal itself produced zero output on-device despite
// the function provably running to completion, so this reads the outcome back out from
// safe, ordinary code instead.
void PollHgsCheckpoint() {
    static int last_value = -1;
    const int value = AetherPS4::Fex::BachataGetHgsCheckpoint();
    if (value != last_value) {
        LOG_CRITICAL(Debug, "MobileOverlayLayer: HandleGuestSignal checkpoint changed {} -> {}",
                     last_value, value);
        last_value = value;
    }
}
#endif
// Last N bytes of the log, not the whole (potentially large, growing-all-session) file --
// plenty for a scrolling panel someone is actively reading, cheap enough to re-read on a
// timer without ever showing up as a frame-time hitch.
constexpr size_t kConsoleTailBytes = 32 * 1024;

// Same file CrashLogger.swift redirects stdout/stderr into (freopen), which every
// LOG_* call already reaches via spdlog's console sink -- so this is the exact same
// content already being sent to Documents for manual inspection all session, just
// tailed live instead of pulled off the device after the fact.
std::string ReadTail(const std::filesystem::path& path, size_t max_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = static_cast<size_t>(file.tellg());
    const auto start = size > max_bytes ? size - max_bytes : size_t{0};
    file.seekg(static_cast<std::streamoff>(start));
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
} // namespace

void MobileOverlayLayer::RefreshConsoleTail() {
    const auto now = ImGui::GetTime();
    if (now - last_refresh_time < kConsoleRefreshIntervalSeconds) {
        return;
    }
    last_refresh_time = now;
    const auto log_path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "aether_crash.log";
    console_tail = ReadTail(log_path, kConsoleTailBytes);
}

void MobileOverlayLayer::DrawToggleButton() {
    // GetMainViewport()'s Pos/Size (not raw io.DisplaySize) are what's actually
    // guaranteed consistent with the coordinate space this backend renders window
    // positions into -- confirmed on-device that using io.DisplaySize directly here
    // put this panel off in the bottom-right corner, partly off-screen, instead of
    // where it was asked to go, on iOS's Retina-scaled display.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 84.0f, viewport->Pos.y + 12.0f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##MobileOverlayToggle", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav)) {
        // The docked game-display window (Display##game_display in vk_presenter.cpp) is
        // begun every frame right after this layer draws, and once real video started
        // rendering it started reliably ending up on top of this floating button,
        // swallowing the tap before ImGui ever registered it as a click. Force this
        // window to the front and give it input priority every single frame instead of
        // only when the user happens to touch it, regardless of whatever else the
        // dockspace decides to draw afterward.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (ImGui::Button(show_panel ? "Hide" : "Console", ImVec2(72.0f, 36.0f))) {
            show_panel = !show_panel;
        }
    }
    ImGui::End();
}

void MobileOverlayLayer::DrawPanel() {
    if (!show_panel) {
        return;
    }

    RefreshConsoleTail();

    const bool booting = !Common::FramePresentedFlag().load(std::memory_order_relaxed);

    // See DrawToggleButton's comment: GetMainViewport(), not raw io.DisplaySize.
    // ImGuiCond_Once (not _Always): confirmed on-device this doesn't do what it looks
    // like -- ImGui persists window positions to imgui.ini (see imgui_core.cpp's
    // SetupSettings/ConfigPath), and "Once" only applies a position if the window has
    // no persisted one yet. A stale position saved from before this centering logic
    // existed silently overrode it on every subsequent launch. _Always forces this
    // panel to recenter every time it opens, which is what's wanted here anyway --
    // nothing about a mobile loading/console card benefits from remembering a
    // previous position across launches.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                    viewport->Pos.y + viewport->Size.y * 0.5f),
                             ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.0f, 520.0f),
                                     std::min(viewport->Size.y - 120.0f, 420.0f)),
                              ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("AetherPS4", &show_panel, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        // See DrawToggleButton's comment: force this above the docked game-display
        // window every frame so its own close ('x') button and scroll area stay
        // touchable once real video is rendering underneath it.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (booting) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Loading game...");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Running");
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Console");
        ImGui::BeginChild("##MobileConsoleScroll", ImVec2(0, 0), ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(console_tail.c_str(), console_tail.c_str() + console_tail.size());
        // Only auto-snap to the bottom when the user was already there (or the panel just
        // opened) -- if they've manually scrolled up to read something, new output
        // shouldn't yank them back down.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void MobileOverlayLayer::Draw() {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    PollHgsCheckpoint();
#endif
    DrawToggleButton();
    DrawPanel();
}

} // namespace Platform::iOS

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
