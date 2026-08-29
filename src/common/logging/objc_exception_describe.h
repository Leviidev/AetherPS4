// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// Isolated from log.cpp specifically so that file can stay plain C++ -- see its own comment
// (in Terminate()) for why compiling it as Objective-C++ directly isn't an option here.
//
// Must be called from within a `catch (...)` block whose currently-active exception may be
// an NSException. Rethrows that exception (via a bare `throw;`, which works correctly even
// across this translation-unit boundary -- it targets whatever exception is currently
// propagating, not something tied to a specific function) and, if it's an NSException,
// fills `out` with its name/reason/backtrace and returns true. Returns false (leaving `out`
// untouched) for any other exception type, including no active exception at all.
bool DescribeCurrentObjCException(std::string& out);
