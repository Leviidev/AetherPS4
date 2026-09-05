// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ios_jit_allocator.cpp — iOS dual-mapped JIT memory implementation.
//
// See ios_jit_allocator.h for full protocol documentation.
//
// This translation unit is only compiled when targeting arm64-apple-ios.
// On all other platforms the header's include guard keeps it inert.

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "core/ios/ios_jit_allocator.h"

#include "common/logging/log.h"

#include <atomic>
#include <chrono>
#include <dlfcn.h>              // dlopen, dlsym
#include <libkern/OSCacheControl.h> // sys_icache_invalidate
#include <mach/mach.h>          // mach_task_self
#include <mach/vm_map.h>        // vm_remap, vm_protect, vm_deallocate (mach_vm.h is unavailable on iOS)
#include <sys/mman.h>           // mmap, munmap, PROT_*, MAP_*
#include <thread>               // std::this_thread::sleep_for
#include <utility>              // std::exchange

// __builtin___clear_cache lowers to a call to this exact symbol on this toolchain rather than
// inlining the AArch64 cache-maintenance instructions directly -- and Apple's real-device iOS
// compiler-rt archive (libclang_rt.ios.a) doesn't provide it (confirmed via nm; oddly, the
// simulator archive libclang_rt.iossim.a does). Every call site needing it in this codebase
// (FEXCore's JIT, the shader recompiler's SRT walker JIT) is compiled for this exact toolchain,
// so providing the symbol ourselves via Darwin's public sys_icache_invalidate -- the same
// primitive CodeCache.cpp already uses -- is simpler and more reliable than trying to coax a
// different compiler-rt archive into the link.
extern "C" void __clear_cache(void* start, void* end) {
    sys_icache_invalidate(start, static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(start)));
}

// BreakpointJIT.framework — the three BRK-trap JIT "syscalls".
//
// This framework is intentionally NOT linked by the Xcode target and must not
// appear in the app's LC_LOAD_DYLIB list. If it is, dyld auto-loads it at
// process launch, before the app runs — and on sideloaded/free-provisioning
// builds AMFI rejects any embedded framework that carries entitlements but
// isn't the main binary ("has entitlements but is not a main binary"),
// crashing the process with SIGABRT during static initialization before
// main() ever runs. Loading it lazily via dlopen()/dlsym() at first use
// avoids the launch-time AMFI check entirely. The framework is still copied
// into the app bundle (via a Run Script build phase, not "Embed & Sign") so
// it's present on disk for this dlopen to find.
namespace {

using BreakGetJITMappingFn = void* (*)(void*, size_t);
using BreakJITDetachFn = void (*)();

struct BreakpointJITSymbols {
    BreakGetJITMappingFn get_jit_mapping = nullptr;
    BreakJITDetachFn jit_detach = nullptr;
};

const BreakpointJITSymbols& GetBreakpointJITSymbols() noexcept {
    static const BreakpointJITSymbols symbols = [] {
        BreakpointJITSymbols s;
        void* handle = dlopen(
            "@executable_path/Frameworks/BreakpointJIT.framework/BreakpointJIT",
            RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            LOG_CRITICAL(Core, "ios_jit_allocator: dlopen(BreakpointJIT) failed: {}",
                         dlerror());
            return s;
        }
        s.get_jit_mapping =
            reinterpret_cast<BreakGetJITMappingFn>(dlsym(handle, "BreakGetJITMapping"));
        s.jit_detach = reinterpret_cast<BreakJITDetachFn>(dlsym(handle, "BreakJITDetach"));
        if (s.get_jit_mapping == nullptr || s.jit_detach == nullptr) {
            LOG_CRITICAL(Core, "ios_jit_allocator: dlsym failed to resolve BreakpointJIT symbols: {}",
                         dlerror());
        }
        return s;
    }();
    return symbols;
}

} // namespace

namespace Core {

// Set (via IosJitTrapGuard below) for exactly the duration of the BreakGetJITMapping call --
// the one place in this file that issues the BRK #0xf00d instruction documented at the top of
// ios_jit_allocator.h. If StikDebug isn't there to service it (killed by iOS's own background
// wake-rate limiter, confirmed on-device via a Console.app capture showing it hit a
// "cpulimit violation" mid-session -- a different cause than the app itself ever calling
// Detach() early, which fex_guest_engine.cpp's own comment already covers), the BRK delivers a
// real, unhandled SIGTRAP that kills the whole process outright -- there is no return from
// get_jit_mapping() to recover from, ios_jit_allocator.h's own doc comment already established
// that. signals.cpp checks this flag to distinguish "this SIGTRAP is the JIT-mapping BRK
// StikDebug failed to intercept" (recoverable: simulate get_jit_mapping() returning nullptr,
// the same failure DualMappedRegion::Allocate() already handles below) from any other SIGTRAP
// (a real debugger breakpoint, which must not be silently swallowed).
//
// A GTA V session hit exactly this fatal path with the crashing thread's own name logged as
// empty ("()") -- not any named guest thread, and not Game:Main, which is what actually called
// Allocate() here. This was thread_local, so it protected only the calling thread; the BRK
// itself, serviced by StikDebug's own BreakpointJIT.framework, evidently executes on an
// internal thread of its own rather than synchronously on the caller (which pthread_kill/BRK's
// normal same-thread-delivery semantics would otherwise guarantee), so that thread's own copy
// of a thread_local flag was never set to true at all -- guaranteeing this crash every time the
// BRK went unserviced, not just occasionally. A plain (non-thread-local) global bool would fix
// the cross-thread case but reintroduces a different race for overlapping concurrent calls to
// Allocate() (one call's guard destructing and clearing the flag while another's request, on a
// different thread, is still in flight and could still trap): reference-counted instead, so
// "expecting a trap" stays true for as long as *any* request is outstanding, however many.
std::atomic<int> g_expecting_jit_mapping_trap_count {0};

class IosJitTrapGuard final {
public:
  IosJitTrapGuard() { g_expecting_jit_mapping_trap_count.fetch_add(1, std::memory_order_acq_rel); }
  ~IosJitTrapGuard() { g_expecting_jit_mapping_trap_count.fetch_sub(1, std::memory_order_acq_rel); }
  IosJitTrapGuard(const IosJitTrapGuard&) = delete;
  IosJitTrapGuard& operator=(const IosJitTrapGuard&) = delete;
};

// ─── DualMappedRegion ────────────────────────────────────────────────────────

// Own counter, separate from FEXCore's (Utils/Allocator.cpp's AllocationCounter) since the two
// live in different static libraries -- both log through the same LogMan/spdlog sink though, so
// the "ios_jit_allocator #N" / "iOSJITAlloc #N" tags together with the log's chronological
// ordering still reconstruct the full cross-library request timeline.
namespace {
std::atomic<uint64_t> g_allocation_counter{0};
}

DualMappedRegion DualMappedRegion::Allocate(size_t bytes) noexcept {
    DualMappedRegion region;
    const uint64_t request_number = g_allocation_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    // Ask the StikDebug-serviced BRK protocol for a FRESH allocation (x0 == 0) to get the
    // EXECUTABLE side of the mapping -- see this file's top-of-file comment for why this is the
    // branch to use, and why a second, local mach_vm_remap is needed for the writable side.
    //
    //    Protocol (JIT26PrepareRegion / brk #0xf00d, x16=1):
    //      x0 = 0, x1 = bytes
    //    Returns: a fresh, debugger-allocated address (now executable) in x0, or nullptr on
    //    failure (BreakpointJIT itself unavailable; a *silent* server-side failure to actually
    //    grant the permission is not distinguishable from success here).
    const auto& symbols = GetBreakpointJITSymbols();
    if (symbols.get_jit_mapping == nullptr) {
        LOG_CRITICAL(Core, "ios_jit_allocator #{}: BreakpointJIT symbols unavailable; cannot allocate JIT region",
                     request_number);
        return region; // invalid
    }

    // A failed request here (nullptr, whether a genuine one from BreakGetJITMapping itself or
    // one simulated by signals.cpp's SIGTRAP handler after an unserviced BRK -- see
    // g_expecting_jit_mapping_trap's comment) doesn't distinguish "StikDebug is permanently
    // gone" from "StikDebug is just momentarily unresponsive" (busy servicing another request,
    // or briefly suspended by iOS before resuming) -- both look identical from here. A few
    // retries with a short backoff costs little (this is already the slow, on-demand path --
    // see flatten_extended_userdata_pass.cpp's own comment on why these calls happen mid-
    // gameplay rather than being front-loaded) and covers the transient case; it does nothing
    // for a StikDebug that's truly dead for the rest of the session, but there's no way to
    // tell those apart in advance, so retrying is strictly no worse than failing immediately.
    constexpr int kMaxAttempts = 3;
    constexpr auto kRetryDelay = std::chrono::milliseconds(50);
    void* rx = nullptr;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        LOG_INFO(Core,
                 "ios_jit_allocator #{}: requesting fresh execute-capable region (size={}) "
                 "attempt={}/{}",
                 request_number, bytes, attempt, kMaxAttempts);
        {
            IosJitTrapGuard trap_guard;
            rx = symbols.get_jit_mapping(nullptr, bytes);
        }
        LOG_INFO(Core, "ios_jit_allocator #{}: BreakGetJITMapping returned {} (execute-side)",
                 request_number, rx);
        if (rx != nullptr) {
            break;
        }
        if (attempt < kMaxAttempts) {
            LOG_WARNING(Core,
                       "ios_jit_allocator #{}: attempt {}/{} failed, retrying after {}ms",
                       request_number, attempt, kMaxAttempts, kRetryDelay.count());
            std::this_thread::sleep_for(kRetryDelay);
        }
    }
    if (rx == nullptr) {
        LOG_CRITICAL(Core,
            "ios_jit_allocator: BreakGetJITMapping(nullptr, {}) returned nullptr after {} "
            "attempts. StikDebug with the Universal JIT Script must be attached before "
            "shadps4_init() is called. See the StikDebug URL-scheme handoff "
            "sequence in JITSupport.swift.",
            bytes, kMaxAttempts);
        return region; // invalid
    }

    // Remap a second, writable virtual alias of the SAME physical pages, purely locally -- no
    // debugger involvement needed for this half (see this file's top-of-file comment).
    // mach_vm.h's 64-bit-explicit API isn't available on iOS ("mach_vm.h unsupported" from the
    // SDK itself) -- vm_map.h's classic API works the same way here since vm_address_t is
    // pointer-width on 64-bit Darwin.
    vm_address_t rw = 0;
    vm_prot_t cur_prot = VM_PROT_NONE, max_prot = VM_PROT_NONE;
    kern_return_t kr = vm_remap(mach_task_self(), &rw, static_cast<vm_size_t>(bytes), /*mask=*/0,
                                 VM_FLAGS_ANYWHERE, mach_task_self(), reinterpret_cast<vm_address_t>(rx),
                                 /*copy=*/FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        LOG_CRITICAL(Core,
            "ios_jit_allocator #{}: vm_remap failed for execute-side {} (size={}): "
            "kern_return_t {} ({})",
            request_number, rx, bytes, static_cast<int>(kr), mach_error_string(kr));
        return region; // invalid
    }
    kr = vm_protect(mach_task_self(), rw, static_cast<vm_size_t>(bytes), /*set_maximum=*/FALSE,
                     VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        LOG_CRITICAL(Core,
            "ios_jit_allocator #{}: vm_protect(RW) failed for remapped {} (size={}): "
            "kern_return_t {} ({})",
            request_number, reinterpret_cast<void*>(rw), bytes, static_cast<int>(kr), mach_error_string(kr));
        vm_deallocate(mach_task_self(), rw, static_cast<vm_size_t>(bytes));
        return region; // invalid
    }

    region.rw_addr = reinterpret_cast<uint8_t*>(rw);
    region.rx_addr = static_cast<uint8_t*>(rx);
    region.size    = bytes;

    LOG_DEBUG(Core,
              "ios_jit_allocator: allocated dual-mapped region: "
              "rw={} rx={} size={}",
              static_cast<void*>(region.rw_addr),
              static_cast<void*>(region.rx_addr),
              region.size);

    return region;
}

void DualMappedRegion::Release() noexcept {
    if (rw_addr != nullptr) {
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(rw_addr), static_cast<vm_size_t>(size));
        rw_addr = nullptr;
    }
    if (rx_addr != nullptr) {
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(rx_addr), static_cast<vm_size_t>(size));
        rx_addr = nullptr;
    }
    size = 0;
}

DualMappedRegion::DualMappedRegion(DualMappedRegion&& o) noexcept
    : rw_addr{std::exchange(o.rw_addr, nullptr)}
    , rx_addr{std::exchange(o.rx_addr, nullptr)}
    , size{std::exchange(o.size, 0)} {}

DualMappedRegion& DualMappedRegion::operator=(DualMappedRegion&& o) noexcept {
    if (this != &o) {
        Release();
        rw_addr = std::exchange(o.rw_addr, nullptr);
        rx_addr = std::exchange(o.rx_addr, nullptr);
        size    = std::exchange(o.size, 0);
    }
    return *this;
}

// ─── IosJitAllocator::Detach ─────────────────────────────────────────────────

namespace IosJitAllocator {

void Detach() noexcept {
    // BRK #0xf00d with x16=0 → JIT26Detach → GDB "D" (detach).
    // Safe to call on non-TXM and TXM devices alike.
    const auto& symbols = GetBreakpointJITSymbols();
    if (symbols.jit_detach == nullptr) {
        LOG_ERROR(Core, "ios_jit_allocator: BreakJITDetach symbol unavailable; skipping detach");
        return;
    }
    LOG_INFO(Core, "ios_jit_allocator: calling BreakJITDetach() — debugger will detach");
    symbols.jit_detach();
    LOG_INFO(Core, "ios_jit_allocator: debugger detached; RX mappings persist");
}

bool IsExpectingJitMappingTrap() noexcept {
    return g_expecting_jit_mapping_trap_count.load(std::memory_order_acquire) > 0;
}

} // namespace IosJitAllocator

} // namespace Core

#endif // defined(__APPLE__) && TARGET_OS_IPHONE
