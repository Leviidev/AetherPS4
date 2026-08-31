// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

// Installs a SIGSYS handler that dumps host/guest register state to stderr before
// chaining to whatever handler was previously installed. FEXCore's guest CPU raises
// SIGSYS on syscall-filter faults, so this is the primary tool for diagnosing JIT
// guest-CPU crashes (bad guest RIP, unhandled syscall) on any platform running the
// FEX guest CPU, not just the original Android runtime target it was written for.
void InstallBachataSigsysTrap();

} // namespace Common
