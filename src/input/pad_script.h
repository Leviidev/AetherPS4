// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Input {

// Harness input: AETHER_PAD_SCRIPT="T:buttons:D,T:buttons:D,..." holds `buttons` (names joined
// with '+': cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options share
// touchpad) on controller 0 from T to T+D seconds after launch. Events may overlap. Drives a
// game to a scene without a person at the controller (Mac harness runs, device tests).
void StartPadScriptFromEnv();

} // namespace Input
