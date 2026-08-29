// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/libs.h"
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

void PS4_SYSV_ABI internal_free(void* pointer) {
    // A pointer reaching plain free() can legitimately have come from either allocator PS4
    // games use through this same libc surface: the host's own malloc() (internal_malloc,
    // above) or one of the game's mspace arenas (SceLibcHeap, sceKernelMapNamedFlexibleMemory
    // -backed -- see internal_sceLibcMspace* above and mspace.cpp). Those aren't the same
    // heap: a pointer carved out of an mspace arena was never a valid host malloc() block, and
    // std::free() unconditionally forwarding it to the host allocator corrupts/aborts that
    // allocator (confirmed on-device: this exact crash, from Journey's own libc freeing a
    // pointer straight out of a freshly-mapped SceLibcHeap arena). Try the mspace arenas
    // first -- MspaceFreeIfOwned is a no-op false for anything not currently live in any of
    // them, so a genuine host-malloc() pointer falls through to std::free() exactly as before.
    if (MspaceFreeIfOwned(pointer)) {
        return;
    }
    // MspaceFreeIfOwned only recognizes a currently-*live* mspace allocation -- "not found"
    // there is also what a double-free or a stale pointer into an already-shrunk/destroyed
    // arena looks like, not just "never came from an mspace arena at all". Those first two
    // cases are still never valid host free() targets (this address was carved out of guest
    // memory, never handed to the host allocator), so check the arena address *ranges*
    // (independent of live/freed state) before concluding this is a genuine host pointer.
    // Confirmed on-device: without this check, exactly this scenario (Journey re-freeing a
    // pointer whose live mspace allocation had already been freed) still reached std::free()
    // and corrupted the host allocator identically to the original bug.
    if (MspaceOwnsAddressRange(pointer)) {
        LOG_WARNING(Lib_LibcInternal,
                    "internal_free: {} falls inside an mspace arena's address range but isn't "
                    "a live allocation there (double-free or stale pointer) -- ignoring rather "
                    "than risking the host allocator",
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
    // Same double-free/stale-pointer gap as internal_free above: a pointer inside an mspace
    // arena's address range that isn't currently a live allocation there is still never a
    // valid host realloc() target.
    if (MspaceOwnsAddressRange(pointer)) {
        LOG_WARNING(Lib_LibcInternal,
                    "internal_realloc: {} falls inside an mspace arena's address range but "
                    "isn't a live allocation there (double-free or stale pointer) -- refusing "
                    "rather than risking the host allocator",
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
