// SPDX-License-Identifier: MIT
#pragma once

#include "Interface/Core/ArchHelpers/Arm64Emitter.h"
#include "Interface/Core/Interpreter/InterpreterOps.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/fextl/memory.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace FEXCore {
struct GuestSigAction;
struct SignalDelegatorConfig;
} // namespace FEXCore

namespace FEXCore::Core {
struct CpuStateFrame;
struct InternalThreadState;
} // namespace FEXCore::Core

namespace FEXCore::Context {
class ContextImpl;
}

namespace FEXCore::CPU {

#define STATE_PTR(STATE_TYPE, FIELD) STATE.R(), offsetof(FEXCore::Core::STATE_TYPE, FIELD)
#define STATE_PTR_IDX(STATE_TYPE, FIELD, INDEX) STATE.R(), ARRAY_OFFSETOF(FEXCore::Core::STATE_TYPE, FIELD, INDEX)
#define FALLBACK_HANDLER_OFFSET(INDEX, FIELD) \
  STATE.R(),                                  \
    (ARRAY_OFFSETOF(FEXCore::Core::CpuStateFrame, Pointers.FallbackHandlerPointers, INDEX) + offsetof(FEXCore::Core::FallbackABIInfo, FIELD))

class Dispatcher final : public Arm64Emitter {
public:
  static fextl::unique_ptr<Dispatcher> Create(FEXCore::Context::ContextImpl* CTX);

  Dispatcher(FEXCore::Context::ContextImpl* ctx);
  ~Dispatcher();

  void InitThreadPointers(FEXCore::Core::InternalThreadState* Thread);

#ifdef VIXL_SIMULATOR
  void ExecuteDispatch(FEXCore::Core::CpuStateFrame* Frame);
  void ExecuteJITCallback(FEXCore::Core::CpuStateFrame* Frame, uint64_t RIP);
#else
  void ExecuteDispatch(FEXCore::Core::CpuStateFrame* Frame) {
    DispatchPtr(Frame, false);
  }

  void ExecuteJITCallback(FEXCore::Core::CpuStateFrame* Frame, uint64_t RIP) {
    CallbackPtr(Frame, RIP);
  }
#endif

  // Defined in Dispatcher.cpp (not inline here) since it needs FEXCore::Allocator's
  // GetExecutableAddress, which AllocatorHooks.h isn't included in this header. This value
  // gets embedded as an absolute literal into every compiled guest block (see
  // Arm64Relocations.cpp's SYMBOL_LITERAL_EXITFUNCTION_LINKER) and later branched to as the
  // block-linking exit stub, so on iOS it must be the executable (RX) address like every
  // other escaping absolute pointer here -- ExitFunctionLinkerAddress itself is the raw
  // write-side value captured via GetCursorAddress() during EmitDispatcher().
  uint64_t GetExitFunctionLinkerAddress() const;

  // Crash-diagnostic only: the dispatcher's own known-good, already-RX-translated entry
  // addresses, for direct comparison against whatever is actually stored in a live thread's
  // CpuStateFrame::Pointers at the moment of a crash (see BachataDumpDispatcherState in
  // fex_guest_engine.cpp). DispatchPtr is the dispatcher's function entry point (its very
  // first emitted byte); DispatcherLoopTopAddress is a separate label further into the same
  // generated code, past the prologue -- these are two genuinely different addresses by
  // construction (confirmed: ~0xa0 bytes apart in EmitDispatcher, matching AbsoluteLoopTop's
  // Bind() coming after several prologue instructions), not two copies of the same value.
  // Compare each live Pointers field against its OWN matching field here, never against
  // DispatchPtr for anything but Pointers.DispatchPtr itself -- an earlier version of this
  // comparison did exactly that (Live.DispatcherLoopTop vs Known.DispatchPtr) and reported a
  // permanent, meaningless "MISMATCH" on every single run, real corruption or not.
  struct DiagnosticAddresses {
    uint64_t DispatchPtr;
    uint64_t Start;
    uint64_t End;
    uint64_t ExitFunctionLinkerAddress;
    uint64_t DispatcherLoopTopAddress;
  };
  DiagnosticAddresses GetDiagnosticAddresses() const;

  SignalDelegatorConfig MakeSignalDelegatorConfig() const;

protected:
  FEXCore::Context::ContextImpl* CTX;

  using AsmDispatch = void (*)(FEXCore::Core::CpuStateFrame* Frame, bool SingleInst);
  using JITCallback = void (*)(FEXCore::Core::CpuStateFrame* Frame, uint64_t RIP);

  AsmDispatch DispatchPtr;
  JITCallback CallbackPtr;
private:
  /**
   * @name Dispatch Helper functions
   * @{ */
  uint64_t ThreadStopHandlerAddress {};
  uint64_t ThreadStopHandlerAddressSpillSRA {};
  uint64_t AbsoluteLoopTopAddress {};
  uint64_t AbsoluteLoopTopAddressFillSRA {};
  uint64_t AbsoluteLoopTopAddressEnterEC {};
  uint64_t AbsoluteLoopTopAddressEnterECFillSRA {};
  uint64_t ThreadPauseHandlerAddress {};
  uint64_t ThreadPauseHandlerAddressSpillSRA {};
  uint64_t ExitFunctionLinkerAddress {};
  uint64_t SignalHandlerReturnAddress {};
  uint64_t SignalHandlerReturnAddressRT {};
  uint64_t GuestSignal_SIGILL {};
  uint64_t GuestSignal_SIGTRAP {};
  uint64_t GuestSignal_SIGSEGV {};

  uint64_t PauseReturnInstruction {};
  std::array<uint64_t, FallbackABI::FABI_UNKNOWN> ABIPointers {};
  /**  @} */

  uint64_t Start {};
  uint64_t End {};

  // Long division helpers
  uint64_t LUDIVHandlerAddress {};
  uint64_t LDIVHandlerAddress {};

  // F64 reduced-precision shared handlers
  uint64_t F64SinHandlerAddress {};
  uint64_t F64CosHandlerAddress {};
  uint64_t F64TanHandlerAddress {};
  uint64_t F64F2XM1HandlerAddress {};
  uint64_t F64ScaleHandlerAddress {};
  uint64_t F64AtanHandlerAddress {};
  uint64_t F64FYL2XHandlerAddress {};
  uint64_t F64FYL2XP1HandlerAddress {};
  uint64_t F64FPREMHandlerAddress {};
  uint64_t F64FPREM1HandlerAddress {};

  void EmitDispatcher();
  uint64_t GenerateABICall(FallbackABI ABI);

  // Inline softfloat conversion emitters - avoid FPCR save/restore overhead
  // These emit ARM64 code that performs the conversion using only integer ops
  void EmitI16ToExtF80();
  void EmitI32ToExtF80();
  void EmitF32ToExtF80();
  void EmitF64ToExtF80();

  // Shared label set for the LUT-based F64 log2 path used by both FYL2X and
  // FYL2XP1. The pool is emitted once via EmitF64Log2Constants.
  struct F64Log2Constants {
    ARMEmitter::ForwardLabel One;
    ARMEmitter::ForwardLabel A0, A1, A2, A3, A4, A5, A6, A7;
    ARMEmitter::ForwardLabel Table;
  };

  void EmitF64Sin();
  void EmitF64Cos();
  void EmitF64Tan();
  void EmitF64F2XM1();
  void EmitF64Scale();
  void EmitF64Atan();
  void EmitF64FYL2X(F64Log2Constants& C);
  void EmitF64FYL2XP1(F64Log2Constants& C);
  void EmitF64Log2Constants(F64Log2Constants& C);
  void EmitF64FPREM();
  void EmitF64FPREM1();

  FEX_CONFIG_OPT(DisableL2Cache, DISABLEL2CACHE);
};

} // namespace FEXCore::CPU
