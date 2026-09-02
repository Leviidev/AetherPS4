// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::LibcInternal {
void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver* sym);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcMemoryAliases(Core::Loader::SymbolsResolver* sym);
#endif

// Exposed so libc_internal_cxa.cpp's C++ operator new/delete HLE entry points can share the
// same allocation path and, critically, the same cross-allocator safety checks as plain C
// malloc()/free() -- see internal_free's own comment (libc_internal_memory.cpp) for why
// blindly forwarding an unrecognized pointer to the host allocator corrupts it.
void* PS4_SYSV_ABI internal_malloc(size_t size);
void PS4_SYSV_ABI internal_free(void* pointer);
} // namespace Libraries::LibcInternal
