// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ios_jit_allocator.h — iOS in-place JIT memory via StikDebug's BreakpointJIT protocol.
//
// Background: iOS enforces W^X strictly and never grants PROT_EXEC to an unentitled
// app through mmap/mprotect. The workaround used by MeloNX/LibRyujinx (and adapted
// here for Bachata-S4) is the "BreakpointJIT" framework: a tiny static framework
// whose three functions (`BreakGetJITMapping`, `BreakMarkJITMapping`, `BreakJITDetach`)
// execute ARM64 BRK-trap instructions that are serviced by an attached external debugger
// (StikDebug running the "Universal JIT Script").
//
// Protocol summary (read from StikDebug's actual source -- github.com/StikDebug/StikDebug,
// `Scripts/universal.js`'s `JIT26PrepareRegion` and `JSSupport/JSDebugSupport.swift`'s
// `handleJITPageWrite`):
//
//   BreakGetJITMapping(addr, len)
//     BRK #0xf00d with x16=1 → JIT26PrepareRegion on debugger side. Two distinct branches:
//     * addr == nullptr ("fresh allocation"): debugger allocates a brand-new region of `len`
//       bytes itself via a GDB-remote "_M<len>,rx" command, then passes THAT address to
//       `prepare_memory_region`. This is the branch actually used here (and by every other
//       JIT'd app confirmed working against this protocol -- Ryujinx/AetherX's
//       DualMappedJitAllocator).
//     * addr != nullptr ("grant an address I already own"): skips the `_M` allocate step and
//       passes the caller's own pre-existing address straight to `prepare_memory_region`. On-
//       device testing traced a real, reproducible execute-permission failure to this branch
//       specifically: the call returns successfully (same address handed back), yet every
//       subsequent attempt to actually branch into that memory faults, forever -- confirmed via
//       raw GDB-remote wire capture showing zero memory-write packets ever sent for it and a
//       stuck fault-resume loop. Do not use this branch.
//     In both branches, `prepare_memory_region` then -- for every 16KB page in the range --
//     sends a raw GDB-remote memory-write command (`M<page_addr>,1:69#checksum`, writing the
//     single byte 0x69) directly to debugserver. The *act* of debugserver performing that
//     out-of-process write is what grants the page execute permission as a side effect
//     (debugserver has elevated privileges an app's own mprotect() doesn't).
//     Returns: the address that ended up executable (the freshly-allocated one, for the
//     branch actually used here), or nullptr if BreakpointJIT itself is unavailable. This
//     address is genuinely execute-only -- confirmed on-device that a plain write to it
//     SIGSEGVs from ordinary host code -- so DualMappedRegion::Allocate() below additionally
//     does a local `mach_vm_remap` to get a second, writable alias of the same physical pages.
//
//   BreakJITDetach()
//     BRK #0xf00d with x16=0 → JIT26Detach → sends GDB "D" (detach).
//     Call once after all JIT mappings are established. Not currently called
//     anywhere in this codebase -- see fex_guest_engine.cpp's comment on why the
//     debugger is kept attached for the whole session (calling this early once
//     caused execute-permission loss, consistent with permission here being tied
//     to the debugger staying attached rather than being a one-time, permanent grant).
//
//   BreakMarkJITMapping(size_t bytes)   ← DO NOT USE in new code.
//     BRK #0x69 (legacy). The universal JIT26 script returns 0xE0000069 for
//     this opcode. Only old, pre-universal-script StikDebug versions handled it.
//
// Usage pattern (call from C++, before the main emulator run loop):
//
//   #if defined(__APPLE__) && TARGET_OS_IPHONE
//   auto region = DualMappedRegion::Allocate(page_size);
//   if (!region) { /* StikDebug not attached, script not running, or remap failed */ abort(); }
//   // Write code through region.rw_addr ...
//   __builtin___clear_cache(region.rw_addr, region.rw_addr + region.size);
//   // ... and execute via region.rx_addr -- a genuinely different virtual address, backed by
//   // the same physical pages (see this file's top comment).
//   #endif
//
// On TXM devices (A15+, M2+): only BreakGetJITMapping works; BreakMarkJITMapping
// is dead. This header/implementation uses BreakGetJITMapping exclusively, so it
// is TXM-safe without any extra branching.

#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <cstddef>
#include <cstdint>

namespace Core {

// A single JIT-capable region, genuinely dual-mapped: rw_addr and rx_addr are two different
// virtual addresses backed by the same physical pages -- rx_addr from BreakGetJITMapping's
// fresh-allocation branch, rw_addr from a local mach_vm_remap of that same memory (see this
// file's top-of-file comment for why both steps are needed).
//
// Lifetime: both mappings are owned by this struct and separately deallocated on destruction
// (mach_vm_deallocate on each address -- they're independent vm_map entries now, not aliases
// of one single mapping).
struct DualMappedRegion {
    uint8_t* rw_addr{nullptr}; ///< Write JIT code here.
    uint8_t* rx_addr{nullptr}; ///< Different address, same physical pages; call/branch to here.
    size_t   size{0};          ///< Length of the mapping, in bytes.

    // Returns true if both pointers are non-null and size > 0.
    [[nodiscard]] bool IsValid() const noexcept {
        return rw_addr != nullptr && rx_addr != nullptr && size > 0;
    }

    // Allocates `size` bytes of JIT-capable memory: BreakGetJITMapping's fresh-allocation
    // branch (addr == nullptr -- see this file's top comment) for the executable side, then a
    // local mach_vm_remap for the writable side.
    //
    // Returns an invalid (nullptr) region if BreakGetJITMapping returns nullptr (script not
    // running, or a JIT26 protocol error while a debugger IS attached), or if the
    // remap/protect step fails (check device logs either way). CALLER MUST already know a
    // debugger is attached before calling this -- if none is, the BRK instruction this issues
    // has no debugger to route to and raises a raw, unhandled SIGTRAP that kills the process
    // outright rather than returning here at all (confirmed on-device: fex_guest_engine.cpp's
    // "Do NOT detach the debugger here" comment documents the same failure mode from a
    // different call site). This was previously (wrongly) documented here as a graceful
    // "StikDebug not attached" case; it is not.
    [[nodiscard]] static DualMappedRegion Allocate(size_t bytes) noexcept;

    // Releases both mappings. Safe to call on an invalid region.
    void Release() noexcept;

    // Non-copyable, moveable.
    DualMappedRegion() noexcept = default;
    ~DualMappedRegion() noexcept { Release(); }
    DualMappedRegion(const DualMappedRegion&) = delete;
    DualMappedRegion& operator=(const DualMappedRegion&) = delete;
    DualMappedRegion(DualMappedRegion&& o) noexcept;
    DualMappedRegion& operator=(DualMappedRegion&& o) noexcept;
};

namespace IosJitAllocator {

// Call once after all DualMappedRegion::Allocate() calls have completed,
// before the emulator's main run loop starts. Instructs the StikDebug debugger
// to detach from the process (BreakJITDetach / BRK #0xf00d, x16=0).
//
// Detaching is safe: the RX mappings already exist in the process's vm_map and
// persist independently of the debugger. After detach the emulator runs free.
//
// If not called: StikDebug stays attached; this isn't fatal but wastes resources
// and may interfere with other debugging tools.
void Detach() noexcept;

// True for exactly the duration of DualMappedRegion::Allocate()'s BreakGetJITMapping call on
// this thread -- see ios_jit_allocator.cpp's g_expecting_jit_mapping_trap for why this exists.
// Called from signals.cpp's SIGTRAP handler to distinguish a StikDebug-unserviced JIT-mapping
// BRK trap (recoverable) from any other SIGTRAP (not).
[[nodiscard]] bool IsExpectingJitMappingTrap() noexcept;

} // namespace IosJitAllocator

} // namespace Core

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
