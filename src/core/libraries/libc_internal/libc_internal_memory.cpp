// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
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
    std::free(pointer);
}

void* PS4_SYSV_ABI internal_realloc(void* pointer, size_t size) {
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
