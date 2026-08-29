// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "libc_internal_memory.h"
#include "mspace.h"

namespace Libraries::LibcInternal {

void* PS4_SYSV_ABI internal_memset(void* s, int c, size_t n) {
    return std::memset(s, c, n);
}

void* PS4_SYSV_ABI internal_memcpy(void* dest, const void* src, size_t n) {
    return std::memcpy(dest, src, n);
}

s32 PS4_SYSV_ABI internal_memcpy_s(void* dest, size_t destsz, const void* src, size_t count) {
#ifdef _WIN64
    return memcpy_s(dest, destsz, src, count);
#else
    std::memcpy(dest, src, count);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_memcmp(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceCreate(const char* name, void* base, size_t capacity,
                                                u32 flags) {
    return MspaceCreate(name, base, capacity, flags);
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceDestroy(void* handle) {
    return MspaceDestroy(handle);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceMalloc(void* handle, size_t size) {
    return MspaceMalloc(handle, size);
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceFree(void* handle, void* pointer) {
    return MspaceFree(handle, pointer);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceCalloc(void* handle, size_t count, size_t size) {
    return MspaceCalloc(handle, count, size);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceRealloc(void* handle, void* pointer, size_t size) {
    return MspaceRealloc(handle, pointer, size);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceMemalign(void* handle, size_t alignment, size_t size) {
    return MspaceMemalign(handle, alignment, size);
}

size_t PS4_SYSV_ABI internal_sceLibcMspaceMallocUsableSize(void* pointer) {
    return MspaceMallocUsableSize(pointer);
}

s32 PS4_SYSV_ABI internal_sceLibcMspacePosixMemalign(void* handle, void** memptr, size_t alignment,
                                                      size_t size) {
    void* result = MspaceMemalign(handle, alignment, size);
    if (result == nullptr) {
        return POSIX_ENOMEM;
    }
    *memptr = result;
    return 0;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceReallocalign(void* handle, void* pointer,
                                                       size_t alignment, size_t size) {
    // No in-place-realloc-with-alignment primitive exists here: allocate fresh at the
    // requested alignment, copy the old contents (bounded by the smaller of the two
    // sizes, since that's all that's guaranteed live), then free the original -- same
    // externally-visible contract as realloc, just alignment-aware.
    void* new_ptr = MspaceMemalign(handle, alignment, size);
    if (new_ptr == nullptr) {
        return nullptr;
    }
    if (pointer != nullptr) {
        const size_t old_size = MspaceMallocUsableSize(pointer);
        std::memcpy(new_ptr, pointer, std::min(old_size, size));
        MspaceFree(handle, pointer);
    }
    return new_ptr;
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceMallocStats(void* handle, void* stats) {
    // No allocator-internal statistics are tracked by this mspace implementation; games
    // that call this to log/report heap stats (not to make allocation decisions) get a
    // zeroed-but-valid result rather than an error.
    return 0;
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceMallocStatsFast(void* handle, void* stats) {
    return 0;
}

void* PS4_SYSV_ABI internal_malloc(size_t size) {
    return std::malloc(size);
}

namespace {
// The two previous fix attempts here both only checked mspace.cpp's own arena tracking
// (MspaceFreeIfOwned's live-allocation lookup, then MspaceOwnsAddressRange's address-range
// membership) and both left the crash completely unchanged. Confirmed why on-device: the
// actual pointer (0x7002230020, logged directly rather than inferred from log adjacency this
// time) sits 0x20 bytes into a region mapped via sceKernelMapNamedFlexibleMemory("SceLibcHeap")
// that was NEVER wrapped in an MspaceCreate() arena at all -- the game's own CRT bootstrap
// evidently manages that region's contents itself, without ever routing through any
// sceLibcMspace* call this codebase intercepts. mspace.cpp's tracking is a strict subset of
// "memory the guest owns"; it was never going to catch this. Check the actual authority instead
// -- the guest VMM itself (the same Core::Memory::VirtualQuery signals.cpp already uses to
// classify fault addresses) -- since ANY address the VMM reports as mapped guest memory is, by
// construction, never a valid host malloc()/free()/realloc() target regardless of which
// in-guest mechanism (mspace arena, raw sceKernelMapNamedFlexibleMemory region, or anything
// else) actually owns it.
bool IsGuestMappedAddress(void* pointer) {
    if (pointer == nullptr) {
        return false;
    }
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr) {
        return false;
    }
    ::Libraries::Kernel::OrbisVirtualQueryInfo info{};
    return memory->VirtualQuery(reinterpret_cast<VAddr>(pointer), 0, &info) == 0;
}
} // namespace

void PS4_SYSV_ABI internal_free(void* pointer) {
    // A pointer reaching plain free() can legitimately have come from either allocator PS4
    // games use through this same libc surface: the host's own malloc() (internal_malloc,
    // above) or guest-owned memory (an mspace arena, or a raw sceKernelMapNamedFlexibleMemory
    // region the game manages itself -- see IsGuestMappedAddress's own comment for how that was
    // found). Those aren't the same heap: guest-owned memory was never a valid host malloc()
    // block, and std::free() unconditionally forwarding it to the host allocator corrupts/
    // aborts that allocator (confirmed on-device, repeatedly, chasing this exact crash). Try
    // the mspace arenas first so a live mspace allocation is actually freed correctly (not just
    // safely ignored), then fall back to the general guest-VMM check for everything mspace.cpp
    // doesn't track -- only once neither claims this address is it safe to assume a genuine
    // host malloc() pointer and fall through to std::free() exactly as before.
    if (MspaceFreeIfOwned(pointer)) {
        return;
    }
    if (MspaceOwnsAddressRange(pointer) || IsGuestMappedAddress(pointer)) {
        LOG_WARNING(Lib_LibcInternal,
                    "internal_free: {} is guest-owned memory (mspace arena or a raw guest "
                    "mapping) but not a live mspace allocation -- ignoring rather than risking "
                    "the host allocator",
                    fmt::ptr(pointer));
        return;
    }
    std::free(pointer);
}

void* PS4_SYSV_ABI internal_realloc(void* pointer, size_t size) {
    // Same cross-allocator hazard as internal_free above, but realloc has no "just don't call
    // it" fallback: a pointer already live in an mspace arena must be resized through that
    // arena (MspaceRealloc, via ResolveArena's registry-wide pointer scan already exposed as
    // MspaceMallocUsableSize) rather than handed to std::realloc(), which would either corrupt
    // the host allocator the same way free() does, or silently succeed against unrelated host
    // heap memory that happens to sit at that address.
    if (MspaceMallocUsableSize(pointer) != 0) {
        return MspaceReallocAnyArena(pointer, size);
    }
    // Same gap internal_free closes above: mspace.cpp's own tracking (both checks) is a strict
    // subset of "memory the guest actually owns" -- fall back to the general guest-VMM check
    // before ever concluding this is a genuine host pointer.
    if (MspaceOwnsAddressRange(pointer) || IsGuestMappedAddress(pointer)) {
        LOG_WARNING(Lib_LibcInternal,
                    "internal_realloc: {} is guest-owned memory (mspace arena or a raw guest "
                    "mapping) but not a live mspace allocation -- refusing rather than risking "
                    "the host allocator",
                    fmt::ptr(pointer));
        return nullptr;
    }
    return std::realloc(pointer, size);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcMemoryAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Q3VBxCXhUHs", "libc", 1, "libc", internal_memcpy);
    LIB_FUNCTION("8zTFvBIAIN8", "libc", 1, "libc", internal_memset);
    LIB_FUNCTION("DfivPArhucg", "libc", 1, "libc", internal_memcmp);
    LIB_FUNCTION("gQX+4GDQjpM", "libc", 1, "libc", internal_malloc);
    LIB_FUNCTION("tIhsqj0qsFE", "libc", 1, "libc", internal_free);
    LIB_FUNCTION("Y7aJ1uydPMo", "libc", 1, "libc", internal_realloc);
}
#endif

void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver* sym) {

    LIB_FUNCTION("NFLs+dRJGNg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy_s);
    LIB_FUNCTION("Q3VBxCXhUHs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy);
    LIB_FUNCTION("8zTFvBIAIN8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memset);
    LIB_FUNCTION("DfivPArhucg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcmp);
    LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceCreate);
    LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceDestroy);
    LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceMalloc);
    LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceFree);
    LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceCalloc);
    LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceRealloc);
    LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceMemalign);
    LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceMallocUsableSize);
    LIB_FUNCTION("qWESlyXMI3E", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspacePosixMemalign);
    LIB_FUNCTION("p6lrRW8-MLY", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceReallocalign);
    LIB_FUNCTION("mfHdJTIvhuo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceMallocStats);
    LIB_FUNCTION("k04jLXu3+Ic", "libSceLibcInternal", 1, "libSceLibcInternal", internal_sceLibcMspaceMallocStatsFast);
}

} // namespace Libraries::LibcInternal
