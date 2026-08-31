// SPDX-License-Identifier: MIT
#pragma once

#include "../guest_cpu/guest_cpu.h"
#include "../guest_cpu/hle_call_adapter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

#ifndef _WIN32
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
bool BachataDumpGuestRegisters(char* out_buf, std::size_t out_buf_size,
                                uint64_t* out_rsp = nullptr) noexcept;

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

private:
  class Impl;

  explicit GuestEngine(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> ImplState;
};

} // namespace AetherPS4::Fex
