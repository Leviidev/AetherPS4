// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

extern "C" {
// Runs the bundled FEXCore ASM correctness-test corpus (prepared by
// runtime/scripts/prepare-fex-asm-tests.py, bundled into the app under
// Resources/fex_asm_tests/) against this app's own iOS-patched FEXCore build --
// same rationale as runtime/probes/fexcore-asm-tests.cpp (see its header comment), but
// running through iOS's real StikDebug-gated dual-mapped JIT path instead of macOS's
// MAP_JIT thread toggle, which is the platform this port actually ships on.
//
// results_path is overwritten with one line per test as it completes (PASS/FAIL/SKIP name:
// detail), flushed after every line -- if a test crashes the whole process (no fork() isolation
// is possible in a single iOS app the way the macOS harness has it; iOS restricts almost
// everything after fork() to exec()/_exit()), the file still holds every result up to and
// including a final "CRASHING <name>" marker written just before that test runs, so the crash
// log and this file together identify exactly which test brought the process down. Returns 0 if
// every test that ran completed with a result written, nonzero on setup failure before any test
// could run.
int shadps4_run_asm_test_corpus(const char* corpus_dir, const char* results_path);
}
