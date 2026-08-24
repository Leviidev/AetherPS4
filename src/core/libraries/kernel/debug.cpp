// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

void PS4_SYSV_ABI sceKernelDebugOutText(void* unk, char* text) {
    sceKernelWrite(1, text, strlen(text));
    return;
}

// Real hardware prints a symbolicated backtrace (with module base/offset info) to the debug
// console; there's no equivalent debug console here worth reproducing that for. Left as a
// pure no-op success -- no arguments read/no pointers dereferenced, since the exact PS4 SDK
// signature isn't needed for that. Confirmed on-device this specific call being unresolved
// (ENOSYS) was fatal, not just a missing diagnostic: RunGuestMain's Execute stage treats any
// FEX HLE failure encountered while running the guest's own entry point as unrecoverable and
// aborts the whole process (Sonic Mania called this during its own startup/exception-handler
// setup), unlike an ENOSYS returned to guest code that's already running normally.
s32 PS4_SYSV_ABI sceKernelPrintBacktraceWithModuleInfo() {
    return ORBIS_OK;
}

void RegisterDebug(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("9JYNqN6jAKI", "libkernel", 1, "libkernel", sceKernelDebugOutText);
    LIB_FUNCTION("Wl2o5hOVZdw", "libkernel", 1, "libkernel", sceKernelPrintBacktraceWithModuleInfo);
}

} // namespace Libraries::Kernel