// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

// Diagnostic-only stopwatch for finding out where game-launch time actually goes -- boot
// time was reported as "takes so long" with no profiler available on-device to measure it
// directly, so this logs real elapsed milliseconds at each major phase instead of guessing.
// MarkBootStart() must be called once, from Emulator::PrepareWindow()'s first line (the
// earliest point common to every launch); BootElapsedMs() is safe to call from any thread
// after that (checkpoints are logged from both the main thread and the guest's own Game:Main
// thread).
void MarkBootStart();
s64 BootElapsedMs();

} // namespace Common
