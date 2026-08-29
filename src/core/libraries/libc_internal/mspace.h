// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Libraries::LibcInternal {

// Returns null for an invalid range or one overlapping a live (or in-flight
// destroyed) arena range.
void* MspaceCreate(const char* name, void* base, std::size_t capacity, u32 flags);
s32 MspaceDestroy(void* handle);
void* MspaceMalloc(void* handle, std::size_t size);
s32 MspaceFree(void* handle, void* pointer);
void* MspaceCalloc(void* handle, std::size_t count, std::size_t size);
void* MspaceRealloc(void* handle, void* pointer, std::size_t size);
void* MspaceMemalign(void* handle, std::size_t alignment, std::size_t size);
// Returns the carved extent of the current live allocation at `pointer` (zero
// byte requests occupy one byte). Pointer values have no historical provenance:
// after legitimate numeric-address reuse, the current live allocation wins.
// The pointer is never dereferenced.
std::size_t MspaceMallocUsableSize(void* pointer);
// Reallocs `pointer` through whichever mspace arena currently owns it (found the same way
// MspaceMallocUsableSize finds an owner), rather than requiring the caller to already know
// the handle. Returns nullptr if no live arena owns it -- the caller (internal_realloc) falls
// back to the host allocator in that case. See MspaceFreeIfOwned's comment for why a pointer's
// owning allocator (host malloc() vs. an mspace arena) has to be established before touching
// it at all.
void* MspaceReallocAnyArena(void* pointer, std::size_t new_size);
// True whenever `pointer` falls inside the address range of any arena this process has ever
// created via MspaceCreate -- including one already destroyed, and independent of whether
// `pointer` currently names a *live* allocation there. MspaceFreeIfOwned/MspaceMallocUsableSize
// only recognize currently-live allocations, so on their own they can't distinguish "never came
// from any mspace arena" from "came from one, but this is a double-free or a stale pointer into
// one" -- both look like "not found" to them, but only the first is actually safe to hand to
// the host allocator. This is the range check that tells them apart.
bool MspaceOwnsAddressRange(void* pointer);
// Frees `pointer` if (and only if) it's a live allocation in some currently-open mspace
// arena, trying every arena the same way MspaceMallocUsableSize does. Returns true if it was
// found and freed, false if no live arena owns it -- the caller (internal_free) falls back to
// the host allocator in that case. Exists because internal_malloc/internal_free forward
// directly to the host's malloc/free, which is only correct for pointers that actually came
// from that same host allocator; a pointer carved out of an mspace arena (the games's own
// SceLibcHeap, sceKernelMapNamedFlexibleMemory-backed, not host-heap-backed) was never a valid
// host malloc() block, and handing it to the host's free() corrupts/aborts the host allocator
// (confirmed on-device: Journey's own libc calling plain free() on such a pointer).
bool MspaceFreeIfOwned(void* pointer);

} // namespace Libraries::LibcInternal
