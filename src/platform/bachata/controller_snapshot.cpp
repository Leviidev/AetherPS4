// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/controller_snapshot.h"

#include <array>
#include <charconv>

namespace Platform::Bachata {
namespace {

constexpr std::string_view Prefix = "BACHATA/1 INPUT ";

template <typename T>
bool ParseNumber(std::string_view text, T& value, int base = 10) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

std::optional<ControllerSnapshot> ParseControllerSnapshot(std::string_view line) {
    if (!line.starts_with(Prefix) || !line.ends_with('\n')) {
        return std::nullopt;
    }
    line.remove_prefix(Prefix.size());
    line.remove_suffix(1);

    ControllerSnapshot result{};
    std::array<bool, 12> seen{};
    std::size_t count = 0;
    while (!line.empty()) {
        const auto separator = line.find(' ');
        const auto token = line.substr(0, separator);
        line = separator == std::string_view::npos ? std::string_view{} : line.substr(separator + 1);
        if (token.empty() || (separator != std::string_view::npos && line.empty())) {
            return std::nullopt;
        }
        const auto equals = token.find('=');
        if (equals == std::string_view::npos || equals == 0 || equals + 1 == token.size()) {
            return std::nullopt;
        }
        const auto key = token.substr(0, equals);
        auto value = token.substr(equals + 1);
        std::size_t index = 0;
        bool valid = false;
        if (key == "slot") {
            index = 0;
            valid = ParseNumber(value, result.slot) && result.slot >= 0 && result.slot <= 3;
        } else if (key == "seq") {
            index = 1;
            valid = ParseNumber(value, result.sequence);
        } else if (key == "buttons") {
            index = 2;
            if (value.starts_with("0x")) {
                value.remove_prefix(2);
                valid = !value.empty() && ParseNumber(value, result.buttons, 16);
            } else {
                valid = ParseNumber(value, result.buttons);
            }
        } else {
            int* target = nullptr;
            int maximum = 255;
            if (key == "lx") index = 3, target = &result.left_x;
            else if (key == "ly") index = 4, target = &result.left_y;
            else if (key == "rx") index = 5, target = &result.right_x;
            else if (key == "ry") index = 6, target = &result.right_y;
            else if (key == "l2") index = 7, target = &result.left_trigger;
            else if (key == "r2") index = 8, target = &result.right_trigger;
            else if (key == "touch") {
                index = 9;
                int touch = 0;
                valid = ParseNumber(value, touch) && touch >= 0 && touch <= 1;
                result.touch_down = touch == 1;
            } else if (key == "tx") index = 10, target = &result.touch_x, maximum = 1919;
            else if (key == "ty") index = 11, target = &result.touch_y, maximum = 1079;
            else return std::nullopt;
            if (target != nullptr) {
                valid = ParseNumber(value, *target) && *target >= 0 && *target <= maximum;
            }
        }
        if (!valid || seen[index]) {
            return std::nullopt;
        }
        seen[index] = true;
        ++count;
    }
    if (count != seen.size() && !(count == seen.size() - 1 && !seen[0])) {
        return std::nullopt;
    }
    return result;
}

} // namespace Platform::Bachata
