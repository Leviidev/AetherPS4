// SPDX-License-Identifier: MIT
#pragma once

#include "../guest_cpu/guest_cpu.h"
#include "../guest_cpu/hle_call_adapter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

namespace AetherPS4::Fex {

enum class EngineStage {
  Request,
  Config,
  Context,
  Mapping,
  Thread,
  Execute,
  Bridge,
  Invalidate,
  Teardown,
};

struct EngineFailure final {
  EngineStage Stage;
  int Error;
};

template <typename T>
using EngineResult = std::variant<T, EngineFailure>;

class GuestBridge {
public:
  virtual ~GuestBridge() = default;

  virtual EngineResult<bool> Invoke(Core::GuestCpu::HleCallFrame& frame) = 0;
  virtual std::optional<Core::GuestExecutionRange> QueryExecutableRange(std::uintptr_t) {
    return std::nullopt;
  }
};

struct GuestRunResult final {
  bool Gpr {};
  bool Rflags {};
  bool Xmm {};
  bool Bridge {};
  bool Threads {};
  bool Tls {};
  bool Unaligned {};
  bool Invalidation {};
};

#ifndef _WIN32
bool HandleGuestSignal(int signal, siginfo_t* info, void* rawContext) noexcept;
// Narrow recovery for a fault whose PC lands exactly on the writable alias of a live JIT
// code buffer instead of its executable alias (a distinct bug from the BUS_ADRALN case
// HandleGuestSignal handles). Only engages for a plain instruction-fetch fault where the
// translated address is confirmed to be inside a live executable code buffer; returns false
// (no state changed) for anything else, including genuine wild guest pointers. See its
// definition for the on-device crash that motivated it.
bool TryRecoverJitAliasFault(int signal, siginfo_t* info, void* rawContext) noexcept;
// Pragmatic, narrowly-targeted recovery for one specific, fully-diagnosed, deterministic
// Rocket League crash (a read of a null-checked-but-invalid pointer while walking what looks
// like Unreal Engine 3's property-linking chain, at one of exactly two known guest addresses
// within one function) -- not a general null-pointer-survival mechanism. See its definition
// for the full diagnostic history (three separate VMM-level hypotheses ruled out with hard
// data) and why the specific recovery (treat the read as if it loaded 0, matching this same
// function's own handling of a legitimately-null chain pointer) is safe here.
bool TryRecoverKnownBadPropertyLink(int signal, siginfo_t* info, void* rawContext) noexcept;
// Recovers a SIGBUS on the guard page just past the *bottom* of FEXCore's call-return
// prediction cache (REG_CALLRET_SP -- see Dispatcher.cpp/BranchOps.cpp) by resetting it back
// to a fresh top and resuming the same instruction, instead of crashing. This cache's push/pop
// are strictly 1:1 with executed x86 call/ret *instructions*, not the guest's real call depth
// -- anything that skips executing a `ret` for one or more frames (C++ exception unwinding via
// a computed jump straight to a landing pad, most notably) leaks that much space permanently,
// so no fixed capacity is ever enough for an arbitrarily long session. Safe to reset outright:
// BranchOps.cpp's own pop already treats a stale/mismatched entry as an ordinary cache miss
// (falls through to the full, slower lookup), never a correctness issue -- only ever engages
// for the bottom guard page specifically, so a fault at the *top* (a different, already-fixed
// bug earlier this session) still falls through to the fatal path instead of being masked here.
bool TryRecoverCallRetStackOverflow(int signal, siginfo_t* info, void* rawContext) noexcept;
// Recovers a fault on a direct-memory address the guest computed against the SDK-standard
// fixed layout (see AddressSpace::TranslateFixedMappingAddress) but that this platform never
// actually backed at that exact address -- confirmed on-device (GTA V, CUSA00419) that
// MapMemory's own rebase makes the *mapping syscall* succeed, real memory backing it
// elsewhere, but the game separately dereferences the original, un-rebased address afterward
// via a plain JIT load/store, since it never learns of the rebase (nothing tracks that the
// game itself will recompute this same address independently rather than remembering whatever
// the syscall returned). Placing real memory at the exact original address isn't achievable
// here (mmap MAP_FIXED: EACCES; a plain hint: silently ignored; vm_allocate VM_FLAGS_FIXED:
// KERN_INVALID_ADDRESS all confirmed refused), so this redirects the access itself instead:
// gated on the fault address itself (from siginfo_t, not a guess) actually falling in one of
// the narrow logical windows TranslateFixedMappingAddress recognizes -- windows this platform
// never legitimately backs any other way, so a fault there is unambiguously this exact
// situation, not a coincidental unrelated crash. Rewrites every live GPR holding a value in
// that same family to its already-backed rebased equivalent and re-executes the same
// instruction (not the next one -- this fixes a source operand a load/store is about to read
// through, unlike the destination-register recoveries elsewhere in this file).
bool TryRecoverDirectMemoryAddressMismatch(int signal, siginfo_t* info, void* rawContext) noexcept;
// Pragmatic, narrowly-targeted recovery for one specific, fully-diagnosed, deterministic GTA V
// (CUSA00419) crash inside a real x86 function at eboot.bin+0x1c08f70 (disassembled directly
// from that function's own byte dump, already captured in an earlier crash's log -- no separate
// extraction needed): a 5-level radix table walk keyed on the function's own 2nd argument (rsi),
// confirmed on-device to be an ordinary pointer inside a direct-memory region this platform's
// own address-rebase fix touches (rsi=0x735806ebe8; the fault address 0x7350 is exactly
// (rsi>>0x18)&0xfff0, matching this walk's own bit-extraction math) -- almost certainly RAGE's
// own internal address -> allocation-metadata lookup, left out of sync with whatever address the
// game later queries it with. None of the walk's 5 levels (or the dereference immediately after)
// null-check before dereferencing. An earlier version of this recovery only patched the first
// faulting load (zero the destination, resume at pc+4): confirmed on-device that this just moves
// the identical crash 18 bytes later to the next unrolled level, and no later level can be
// patched that way at all once execution reaches the `cmp`+conditional-branch a few instructions
// after the walk -- skipping a compare corrupts the flags its branch depends on. This version
// instead recovers the whole lookup at once: simulates the function returning 0 (not-found) by
// writing the guest CPU state directly and redirecting host pc to
// Pointers.DispatcherLoopTopFillSRA -- FEXCore's own "resume guest execution at an arbitrary
// address, refilling every SRA register from Frame->State first" entry point, the same class of
// redirect SafepointSignalHandler already performs from a signal handler for a different bug.
// Landing on this function's own clean epilogue is safe because nothing it pushed has been
// touched -- the fault is a pure read partway through the walk. Gives up and falls through to
// the real crash handler if this ever fires enough times in a row to suggest a spin-retry loop
// rather than isolated lookup misses.
//
// Confirmed on-device that an earlier version of this fix, which only wrote
// Frame->State.gregs[REG_RAX] before the DispatcherLoopTopFillSRA redirect, corrupted execution:
// FillSRA reloads *every* SRA-mapped register from Frame->State, which is a stale HLE/JIT-block
// checkpoint, not the live ARM64 state -- leaving Frame->State.gregs[REG_RSP] stale meant the
// redirected epilogue's pop sequence read off the wrong stack location entirely, producing a
// wild jump (to 0xDEADBEEF54321ABC, this codebase's own dummy stack-guard value, landing where a
// real guest RIP should be) a few instructions later. Fixed by syncing every SRA-mapped register
// from the live ARM64 state into Frame->State before the redirect, so FillSRA's reload is a
// no-op for everything except the one register (RAX) this recovery deliberately overrides.
bool TryRecoverNullResourceTableLookup(int signal, siginfo_t* info, void* rawContext) noexcept;
bool TryRecoverNullResourceTableLookup(int signal, siginfo_t* info, void* rawContext) noexcept;
// Queue Orbis guest exception handler for deferred FEX delivery (ARM64 host).
// orbis_sig is the Orbis signal number (e.g. 30 / SIGUSR1). guest_handler is the
// guest VA from Libraries::Kernel::Handlers. Actual run is HandleCallback at HLE
// boundary — mirrors desktop Windows APC ExceptionHandler without host inject.
bool DeliverGuestOrbisSignal(int orbis_sig, siginfo_t* info, void* rawContext,
                             std::uintptr_t guest_handler) noexcept;

// Async-signal-safe best-effort query of the current guest RIP and syscall number
// (RAX) from the active FEX thread context. Writes nothing and returns false if no
// FEX thread is active on the current host thread. Designed for use inside a SIGSYS
// signal handler — touches only thread-local pointer state and frame registers.
bool BachataQueryGuestRipSyscall(uint64_t* out_rip, uint64_t* out_syscall) noexcept;

// Crash-diagnostic only: classifies a host fault address (the SIGSEGV/SIGBUS si_addr) against
// FEXCore's iOS dual-mapped JIT allocation tables. Writes a short human-readable description
// into out_buf (always null-terminated if out_buf_size > 0) and returns true if the diagnostic
// was available at all (iOS build with FEX guest CPU enabled); false otherwise, in which case
// out_buf is left untouched. Safe to call from the crash path -- not async-signal-safe (takes a
// mutex internally), but this is only ever reached right before the process aborts anyway, same
// as the existing Common::ReportCrash/LOG_CRITICAL calls at that point.
bool BachataDescribeHostFaultAddress(void* fault_addr, char* out_buf, std::size_t out_buf_size) noexcept;

// Crash-diagnostic only: writes a comparison of the dispatcher's own known-good entry
// addresses against what the currently-active FEX thread's live CpuStateFrame::Pointers
// actually holds for the same fields (see ContextImpl::DumpDispatcherStateForDiagnostics).
// Returns false (out_buf untouched) if no FEX thread is active on the current host thread.
bool BachataDumpDispatcherState(char* out_buf, std::size_t out_buf_size) noexcept;

// Crash-diagnostic only: dumps every x86-64 GPR (including RSP/RBP) from the currently-active
// FEX thread's spilled CpuStateFrame::State, same data source as BachataQueryGuestRipSyscall
// above. Added specifically because a bare rip/rax pair wasn't enough to tell what a
// NULL-pointer guest write actually came from. out_rsp (optional) additionally receives RSP
// as a raw value -- separate from the formatted text in out_buf -- so the caller can walk raw
// guest stack words (return addresses into eboot.bin) for a poor-man's backtrace without
// needing guest debug symbols. Returns false (out_buf/out_rsp untouched) if no FEX thread is
// active on the current host thread.
// out_rbp (optional) additionally receives RBP as a raw value, the same way out_rsp does --
// lets a caller with its own VMM access do a real [rbp]/[rbp+8] frame-pointer walk instead of
// the RSP-scan heuristic above, for functions (confirmed via disassembly to push rbp; mov rbp,
// rsp at entry) that actually maintain one.
bool BachataDumpGuestRegisters(char* out_buf, std::size_t out_buf_size, uint64_t* out_rsp = nullptr,
                                uint64_t* out_rbp = nullptr) noexcept;

// Crash-diagnostic only: dumps the raw ARM64 32-bit words surrounding fault_pc (8 before, the
// faulting word itself bracketed in [], 8 after) as hex, reading through the same
// writable-alias translation HandleGuestSignal's backpatch uses (fault_pc, the host PC where
// the fatal signal landed, is the execute-only side of iOS's dual JIT mapping and not directly
// readable). Unlike BachataQueryGuestRipSyscall's guest RIP -- which is only the last JIT/HLE
// checkpoint, not necessarily the live fault site -- fault_pc from the signal context is exact.
// Fixed-width ARM64 instructions make this safe to hand-decode without a disassembler, unlike
// the variable-length x86 case. Returns false (out_buf untouched) outside iOS/FEX builds.
bool BachataDumpHostCodeWords(void* fault_pc, char* out_buf, std::size_t out_buf_size) noexcept;

// Crash-diagnostic only: reconstructs the *exact* guest RIP that was executing at fault_pc, using
// FEXCore's own per-block RIP-entries side table (FEXCore::Context::Context::RestoreRIPFromHostPC)
// rather than the last stale JIT/HLE checkpoint BachataQueryGuestRipSyscall reports. Needed for
// faults that land deep inside a large, branch-free block, where the checkpoint RIP is just the
// block's entry point, nowhere near the actual faulting instruction. Returns false (out_rip
// untouched) if no FEX thread is active on the current host thread.
bool BachataReconstructAccurateGuestRIP(void* fault_pc, uint64_t* out_rip) noexcept;

// Diagnostic-only pair for one specific, known-deterministic guest block (see the matching call
// in Core.cpp's ContextImpl::CompileBlock). Record stores a snapshot of the freshly compiled
// bytes at compile time; Compare re-reads the same address later (from the crash path) and
// reports byte-for-byte whether it's still identical or something rewrote it in between. Not a
// general mechanism -- a single, process-wide slot for the one address currently under
// investigation.
void BachataRecordKnownBlockSnapshot(uintptr_t writable_code_ptr, const unsigned char* bytes, std::size_t len) noexcept;
bool BachataCompareKnownBlockSnapshot(char* out_buf, std::size_t out_buf_size) noexcept;

// Deliver any Orbis guest signal queued by DeliverGuestOrbisSignal for the
// current thread, running its handler via nested HandleCallback at this safe HLE
// point. No-op when nothing is pending or no FEX guest thread is active. Blocking
// HLE waits (semaphore/cond) call this between wait chunks so stop-the-world
// signals (e.g. Unity GC, signal 30) reach threads parked in a host futex that
// would otherwise swallow the interrupting EINTR inside libstdc++.
void FlushPendingGuestOrbisSignal() noexcept;

// Diagnostic-only checkpoint counter updated via plain atomic store (no I/O, no
// allocation -- genuinely signal-safe, unlike every write()/vsnprintf-based logging
// attempt tried directly inside HandleGuestSignal, all of which produced zero output
// on-device despite the function definitely running to completion without crashing).
// Meant to be polled from an unrelated, healthy thread (e.g. once per frame from the
// render loop) and logged normally there once it changes -- see the numbered comments
// at each store site in fex_guest_engine.cpp for what each value means.
int BachataGetHgsCheckpoint() noexcept;
#endif

class GuestEngine final {
public:
  class Thread;

  static EngineResult<std::unique_ptr<GuestEngine>> Create(GuestBridge& bridge);

  GuestEngine(const GuestEngine&) = delete;
  GuestEngine& operator=(const GuestEngine&) = delete;
  ~GuestEngine();

  EngineResult<GuestRunResult> RunControlledHarness();
  EngineResult<Thread*> CreateThread(const Core::GuestExecutionRequest& request);
  EngineResult<Core::GuestExecutionState> Run(Thread& thread);
  EngineResult<Core::GuestExecutionState> CallGuest(std::uintptr_t rip,
                                               std::span<const std::uint64_t> arguments);
  EngineResult<bool> Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size);
  EngineResult<bool> DestroyThread(Thread*& thread);
  EngineResult<bool> Shutdown();
  std::uintptr_t ReturnAddress() const;
  Core::GuestExecutionRange ReturnRange() const;
  Core::GuestExecutionRange CallbackReturnRange() const;

  // Diagnostic only (GTA V stall investigation): lists every currently-registered guest
  // thread's native handle and name. raw_impl must be an Impl* obtained by a prior GuestEngine
  // instance storing itself via g_guest_engine_impl_for_diagnostics (fex_guest_engine.cpp) --
  // exposed as a public static taking a type-erased pointer, rather than an instance method,
  // because the only caller (HleStallWatchdogThread) is a detached background thread with no
  // GuestEngine& of its own, only that raw pointer.
  static std::string DumpThreadNamesForDiagnostics(void* raw_impl);

  // Diagnostic only (GTA V stall investigation, continued): finds the native pthread_t handle
  // for the first currently-registered guest thread whose name exactly matches `name`, or
  // pthread_t{} if none is registered under that name right now. Same raw-pointer contract as
  // DumpThreadNamesForDiagnostics above, for the same reason (only caller is a detached
  // background thread). Lets that caller pthread_kill() a specific named thread (e.g.
  // "Game:Main") with a diagnostic-only signal to sample its live guest RIP, rather than only
  // ever seeing whatever it last logged -- Game:Main went completely silent for the rest of a
  // GTA V session right after spawning its render thread, with no way to tell from logging
  // alone whether it was still alive, or where.
  static pthread_t FindGuestThreadHandleByName(void* raw_impl, const char* name);

private:
  class Impl;

  explicit GuestEngine(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> ImplState;
};

} // namespace AetherPS4::Fex
