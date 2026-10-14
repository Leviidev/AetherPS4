// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ios_jit_allocator.cpp — iOS dual-mapped JIT memory implementation.
//
// See ios_jit_allocator.h for full protocol documentation.
//
// Compiled for the phone path (AETHER_PHONE_PATH): iOS itself, and the Mac harness that runs the
// phone's code. On the Mac the execute side comes from the process itself (see
// MacGetJITMapping below) instead of StikDebug; everything after that is the same code.

#include "common/phone_path.h"

#if defined(AETHER_PHONE_PATH)

#include "core/ios/ios_jit_allocator.h"

#include "common/logging/log.h"
#if defined(AETHER_FEX_HAS_JIT_HOST)
#include <FEXCore/Utils/AllocatorHooks.h>
#endif

#include <atomic>
#include <chrono>
#include <dlfcn.h>              // dlopen, dlsym
#include <libkern/OSCacheControl.h> // sys_icache_invalidate
#include <mach/mach.h>          // mach_task_self
#include <mach/vm_map.h>        // vm_remap, vm_protect, vm_deallocate (mach_vm.h is unavailable on iOS)
#include <sys/mman.h>           // mmap, munmap, PROT_*, MAP_*
#include <csetjmp>
#include <csignal>
#include <cstring>
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
#if TARGET_OS_IPHONE
extern "C" void __clear_cache(void* start, void* end) {
    sys_icache_invalidate(start, static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(start)));
}
#endif

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

#if !TARGET_OS_IPHONE
// The Mac harness stands in for StikDebug: a process that is not hardened may map anonymous
// read+execute pages itself, and DualMappedRegion::Allocate then makes the writable alias with
// vm_remap exactly as it does for StikDebug's pages on the phone. MAP_JIT is not used: a MAP_JIT
// region cannot be vm_remap'd (KERN_INVALID_ADDRESS), and the phone has no MAP_JIT either.
void* MacGetJITMapping(void* addr, size_t len) {
    if (addr != nullptr) {
        return nullptr; // the phone's grant-in-place branch does not work there either
    }
    void* rx = mmap(nullptr, len, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    return rx == MAP_FAILED ? nullptr : rx;
}

void MacJITDetach() {}

sigjmp_buf g_selftest_jump;
volatile sig_atomic_t g_selftest_running = 0;
void SelftestFault(int sig) {
    if (g_selftest_running) {
        siglongjmp(g_selftest_jump, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// Writes `movz w0, #0x5a5a; ret` through the writable alias and calls it through the executable
// side, so a Mac that refuses this layout (a hardened build, say) fails here with a message
// instead of with a SIGBUS somewhere inside the JIT later.
bool SelftestPair(uint8_t* rw, uint8_t* rx) {
    static const uint32_t code[2] = {0x528B4B40u, 0xD65F03C0u};
    struct sigaction sa {}, old_bus {}, old_segv {}, old_ill {};
    sa.sa_handler = SelftestFault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGILL, &sa, &old_ill);
    bool ok = false;
    g_selftest_running = 1;
    if (sigsetjmp(g_selftest_jump, 1) == 0) {
        std::memcpy(rw, code, sizeof(code));
        sys_icache_invalidate(rx, sizeof(code));
        ok = reinterpret_cast<int (*)()>(rx)() == 0x5a5a;
        std::memset(rw, 0, sizeof(code));
        sys_icache_invalidate(rx, sizeof(code));
    }
    g_selftest_running = 0;
    sigaction(SIGBUS, &old_bus, nullptr);
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGILL, &old_ill, nullptr);
    return ok;
}
#endif

const BreakpointJITSymbols& GetBreakpointJITSymbols() noexcept {
#if !TARGET_OS_IPHONE
    static const BreakpointJITSymbols mac_symbols{MacGetJITMapping, MacJITDetach};
    return mac_symbols;
#endif
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

// See IosJitAllocator::MarkStikDebugLikelyDead()'s own doc comment (ios_jit_allocator.h).
std::atomic<bool> g_stikdebug_likely_dead {false};

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

namespace {
JitHost g_jit_host{};
std::atomic<bool> g_has_jit_host{false};
} // namespace

void SetJitHost(const JitHost& host) noexcept {
    g_jit_host = host;
    g_has_jit_host.store(host.alloc != nullptr && host.free != nullptr, std::memory_order_release);
#if defined(AETHER_FEX_HAS_JIT_HOST)
    FEXCore::Allocator::SetJITHostHooks(FEXCore::Allocator::JITHostHooks{
        .Alloc = host.alloc,
        .Free = host.free,
        .Delta = host.delta,
        .RWBase = host.rw_base,
        .Size = host.size,
    });
#endif
}

bool HasJitHost() noexcept {
    return g_has_jit_host.load(std::memory_order_acquire);
}

DualMappedRegion DualMappedRegion::Allocate(size_t bytes) noexcept {
    DualMappedRegion region;
    const uint64_t request_number = g_allocation_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    if (HasJitHost()) {
        void* rx = nullptr;
        void* rw = g_jit_host.alloc(bytes, &rx);
        if (rw == nullptr || rx == nullptr) {
            LOG_CRITICAL(Core, "ios_jit_allocator #{}: the host refused {} KiB of JIT memory",
                         request_number, bytes >> 10);
            return region; // invalid
        }
        region.rw_addr = static_cast<uint8_t*>(rw);
        region.rx_addr = static_cast<uint8_t*>(rx);
        region.size = bytes;
        LOG_INFO(Core, "ios_jit_allocator #{}: host gave {} KiB (rw={} rx={})", request_number,
                 bytes >> 10, rw, rx);
        return region;
    }

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

#if !TARGET_OS_IPHONE
    static std::atomic<int> s_selftest{0}; // 0 not run, 1 passed, 2 failed
    if (s_selftest.load(std::memory_order_acquire) == 0) {
        const bool ok = bytes >= 8 && SelftestPair(region.rw_addr, region.rx_addr);
        s_selftest.store(ok ? 1 : 2, std::memory_order_release);
        LOG_INFO(Core, "ios_jit_allocator: Mac phone-path JIT pair self-test {} (rw={} rx={})",
                 ok ? "passed" : "FAILED", static_cast<void*>(region.rw_addr),
                 static_cast<void*>(region.rx_addr));
    }
    if (s_selftest.load(std::memory_order_acquire) != 1) {
        LOG_CRITICAL(Core, "ios_jit_allocator: this Mac process cannot run code written through a "
                           "writable alias (hardened runtime?); refusing JIT region");
        region.Release();
        return region; // invalid
    }
#endif

    LOG_DEBUG(Core,
              "ios_jit_allocator: allocated dual-mapped region: "
              "rw={} rx={} size={}",
              static_cast<void*>(region.rw_addr),
              static_cast<void*>(region.rx_addr),
              region.size);

    return region;
}

void DualMappedRegion::Release() noexcept {
    if (HasJitHost() && rw_addr != nullptr) {
        g_jit_host.free(rw_addr, size);
        rw_addr = nullptr;
        rx_addr = nullptr;
        size = 0;
        return;
    }
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
    if (HasJitHost()) {
        return; // the host owns the debugger session, if there is one
    }
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

void MarkStikDebugLikelyDead() noexcept {
    g_stikdebug_likely_dead.store(true, std::memory_order_release);
}

bool IsStikDebugLikelyDead() noexcept {
    return g_stikdebug_likely_dead.load(std::memory_order_acquire);
}

} // namespace IosJitAllocator

} // namespace Core

#endif // defined(AETHER_PHONE_PATH)
