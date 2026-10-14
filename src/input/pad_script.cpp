// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/pad_script.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"

namespace Input {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

struct ScriptEvent {
    double start;
    double end;
    u32 buttons;
};

u32 ButtonFromName(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, OrbisPadButtonDataOffset>, 17> names{{
        {"cross", OrbisPadButtonDataOffset::Cross},
        {"circle", OrbisPadButtonDataOffset::Circle},
        {"square", OrbisPadButtonDataOffset::Square},
        {"triangle", OrbisPadButtonDataOffset::Triangle},
        {"up", OrbisPadButtonDataOffset::Up},
        {"down", OrbisPadButtonDataOffset::Down},
        {"left", OrbisPadButtonDataOffset::Left},
        {"right", OrbisPadButtonDataOffset::Right},
        {"l1", OrbisPadButtonDataOffset::L1},
        {"r1", OrbisPadButtonDataOffset::R1},
        {"l2", OrbisPadButtonDataOffset::L2},
        {"r2", OrbisPadButtonDataOffset::R2},
        {"l3", OrbisPadButtonDataOffset::L3},
        {"r3", OrbisPadButtonDataOffset::R3},
        {"options", OrbisPadButtonDataOffset::Options},
        {"share", OrbisPadButtonDataOffset::Share},
        {"touchpad", OrbisPadButtonDataOffset::TouchPad},
    }};
    for (const auto& [n, button] : names) {
        if (n == name) {
            return static_cast<u32>(button);
        }
    }
    return 0;
}

std::vector<ScriptEvent> ParseScript(const std::string& script) {
    std::vector<ScriptEvent> events;
    std::stringstream entries(script);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        const auto first = entry.find(':');
        const auto second = entry.find(':', first == std::string::npos ? first : first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            LOG_ERROR(Input, "AETHER_PAD_SCRIPT: ignoring '{}' (want T:buttons:D)", entry);
            continue;
        }
        const double start = std::atof(entry.substr(0, first).c_str());
        const double hold = std::atof(entry.substr(second + 1).c_str());
        u32 buttons = 0;
        std::stringstream names(entry.substr(first + 1, second - first - 1));
        std::string name;
        while (std::getline(names, name, '+')) {
            const u32 button = ButtonFromName(name);
            if (button == 0) {
                LOG_ERROR(Input, "AETHER_PAD_SCRIPT: unknown button '{}'", name);
            }
            buttons |= button;
        }
        events.push_back({start, start + hold, buttons});
    }
    return events;
}

} // namespace

void StartPadScriptFromEnv() {
    const char* script = std::getenv("AETHER_PAD_SCRIPT");
    if (script == nullptr || *script == '\0') {
        return;
    }
    auto events = ParseScript(script);
    if (events.empty()) {
        return;
    }
    LOG_INFO(Input, "AETHER_PAD_SCRIPT: {} events", events.size());
    std::thread([events = std::move(events)] {
        using Clock = std::chrono::steady_clock;
        const auto begin = Clock::now();
        double last_end = 0.0;
        for (const auto& event : events) {
            last_end = std::max(last_end, event.end);
        }
        auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
        // Sticks centred, triggers released.
        const std::array<int, 6> axes{128, 128, 128, 128, 0, 0};
        // Nothing is applied until the first press: ApplyRemoteState reads the PS4 process
        // clock, which the emulator has not set up yet at launch.
        u32 applied = 0;
        for (;;) {
            const double now = std::chrono::duration<double>(Clock::now() - begin).count();
            u32 buttons = 0;
            for (const auto& event : events) {
                if (now >= event.start && now < event.end) {
                    buttons |= event.buttons;
                }
            }
            if (buttons != applied) {
                LOG_INFO(Input, "AETHER_PAD_SCRIPT: t={:.2f}s buttons={:#x}", now, buttons);
                (*controllers)[0]->ApplyRemoteState(static_cast<OrbisPadButtonDataOffset>(buttons),
                                                    axes, false, 0.0f, 0.0f);
                applied = buttons;
            }
            if (now > last_end + 1.0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }).detach();
}

} // namespace Input
