// SPDX-License-Identifier: MIT
/*
$info$
glossary: Splatter ~ a code generator backend that concatenates configurable macros instead of doing isel
glossary: IR ~ Intermediate Representation, our high-level opcode representation, loosely modeling arm64
glossary: SSA ~ Single Static Assignment, a form of representing IR in memory
glossary: Basic Block ~ A block of instructions with no control flow, terminated by control flow
glossary: Fragment ~ A Collection of basic blocks, possibly an entire guest function or a subset of it
tags: backend|arm64
desc: Main glue logic of the arm64 splatter backend
$end_info$
*/

#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/Dispatcher/Dispatcher.h"
#include "Interface/Core/Interpreter/InterpreterOps.h"
#include "Interface/Core/JIT/DebugData.h"
#include "Interface/Core/JIT/JITClass.h"
#include "Interface/IR/Passes/RegisterAllocationPass.h"

#include "Utils/MemberFunctionToPointer.h"
#include "Utils/variable_length_integer.h"

#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/EnumUtils.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/Telemetry.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

namespace {
struct DivRem {
  uint64_t Quotient;
  uint64_t Remainder;
};

static struct DivRem LUDIV(uint64_t SrcHigh, uint64_t SrcLow, uint64_t Divisor) {
  __uint128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;

  return {
    .Quotient = (uint64_t)(Source / Divisor),
    .Remainder = (uint64_t)(Source % Divisor),
  };
}

static struct DivRem
LDIV(uint64_t SrcHigh, uint64_t SrcLow, int64_t Divisor) {
  __int128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;

  return {
    .Quotient = (uint64_t)(Source / Divisor),
    .Remainder = (uint64_t)(Source % Divisor),
  };
}

static void
PrintValue(uint64_t Value) {
  LogMan::Msg::DFmt("Value: 0x{:x}", Value);
}

static void PrintMsg(const char* Value) {
  LogMan::Msg::DFmt("{}", Value);
}

static void PrintVectorValue(uint64_t Value, uint64_t ValueUpper) {
  LogMan::Msg::DFmt("Value: 0x{:016x}'{:016x}", ValueUpper, Value);
}
} // namespace

namespace FEXCore::CPU {

void Arm64JITCore::Op_Unhandled(const IR::IROp_Header* IROp, IR::Ref Node) {
  FallbackInfo Info;
  if (!InterpreterOps::GetFallbackHandler(IROp, &Info)) {
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    LOGMAN_MSG_A_FMT("Unhandled IR Op: {}", FEXCore::IR::GetName(IROp->Op));
#endif
  } else {
    auto FillF80x2Result = [&](auto DstLo, auto DstHi) {
      mov(DstLo.Q(), VTMP1.Q());
      mov(DstHi.Q(), VTMP2.Q());
    };

    auto FillF64x2Result = [&](auto DstLo, auto DstHi) {
      fmov(DstLo.D(), VTMP1.D());
      fmov(DstHi.D(), VTMP2.D());
    };

    auto FillF80Result = [&]() {
      const auto Dst = GetVReg(Node);
      mov(Dst.Q(), VTMP1.Q());
    };

    auto FillF64Result = [&]() {
      const auto Dst = GetVReg(Node);
      fmov(Dst.D(), VTMP1.D());
    };

    auto FillF32Result = [&]() {
      const auto Dst = GetVReg(Node);
      fmov(Dst.S(), VTMP1.S());
    };

    auto FillI64Result = [&]() {
      const auto Dst = GetReg(Node);
      mov(Dst.X(), TMP1);
    };

    auto FillI32Result = [&]() {
      const auto Dst = GetReg(Node);
      mov(Dst.W(), TMP1.W());
    };

    auto FillI16Result = [&]() {
      const auto Dst = GetReg(Node);
      mov(Dst.W(), TMP1.W());
    };

    switch (Info.ABI) {
    case FABI_F80_I16_F32_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      fmov(VTMP1.S(), Src1.S());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80Result();
    } break;

    case FABI_F80_I16_F64_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      fmov(VTMP1.D(), Src1.D());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80Result();
    } break;

    case FABI_F80_I16_I16_PTR:
    case FABI_F80_I16_I32_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // tmp2 (x1/x11): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetReg(IROp->Args[0]);

      // Need to sign or zero extend this for the dispatcher handler.
      if (Info.ABI == FABI_F80_I16_I16_PTR) {
        sxth(ARMEmitter::Size::i32Bit, TMP2, Src1);
      } else {
        mov(ARMEmitter::Size::i32Bit, TMP2, Src1);
      }

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80Result();
    } break;

    case FABI_F32_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF32Result();
    } break;

    case FABI_F64_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF64Result();
    } break;

    case FABI_F64_F64_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      fmov(VTMP1.D(), Src1.D());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF64Result();
    } break;
    case FABI_F64x2_F64_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source
      // vtmp2 (v1/v16): vector source
#ifdef VIXL_SIMULATOR
      LOGMAN_THROW_A_FMT(CTX->Config.DisableVixlIndirectCalls, "Vector register pairs unsupported by simulator currently");
#endif
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      const auto DstLo = GetVReg(IROp->Args[1]);
      const auto DstHi = GetVReg(IROp->Args[2]);

      fmov(VTMP1.D(), Src1.D());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF64x2Result(DstLo, DstHi);
    } break;

    case FABI_F64_F64_F64_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v17): vector source 2
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      const auto Src2 = GetVReg(IROp->Args[1]);

      fmov(VTMP1.D(), Src1.D());
      fmov(VTMP2.D(), Src2.D());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF64Result();
    } break;

    case FABI_I16_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI16Result();
    } break;

    case FABI_I32_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI32Result();
    } break;

    case FABI_I64_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): source
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI64Result();
    } break;

    case FABI_I64_I16_F80_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v17): vector source 2
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      const auto Src2 = GetVReg(IROp->Args[1]);
      mov(VTMP1.Q(), Src1.Q());
      mov(VTMP2.Q(), Src2.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI64Result();
    } break;

    case FABI_F80_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80Result();
    } break;

    case FABI_F80x2_I16_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v16): vector source 2
#ifdef VIXL_SIMULATOR
      LOGMAN_THROW_A_FMT(CTX->Config.DisableVixlIndirectCalls, "Vector register pairs unsupported by simulator currently");
#endif
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      const auto DstLo = GetVReg(IROp->Args[1]);
      const auto DstHi = GetVReg(IROp->Args[2]);

      mov(VTMP1.Q(), Src1.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80x2Result(DstLo, DstHi);
    } break;

    case FABI_F80_I16_F80_F80_PTR: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v17): vector source 2
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(IROp->Args[0]);
      const auto Src2 = GetVReg(IROp->Args[1]);

      mov(VTMP1.Q(), Src1.Q());
      mov(VTMP2.Q(), Src2.Q());

      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP1);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillF80Result();
    } break;

    case FABI_I32_I64_I64_V128_V128_I16: {
      // Linux Reg/Win32 Reg:
      // stack: FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v17): vector source 2
      // tmp1 (x0/x10): source 1
      // tmp2 (x1/x11): source 2
      // tmp3 (x2/x12): source 3
      const auto Op = IROp->C<IR::IROp_VPCMPESTRX>();
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP1, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));

      stp<ARMEmitter::IndexType::PRE>(TMP1, ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto SrcRAX = GetReg(Op->RAX);
      const auto SrcRDX = GetReg(Op->RDX);
      const auto Control = Op->Control;

      mov(TMP1, SrcRAX.X());
      mov(TMP2, SrcRDX.X());
      movz(ARMEmitter::Size::i32Bit, TMP3, Control);

      const auto Src1 = GetVReg(Op->LHS);
      const auto Src2 = GetVReg(Op->RHS);

      mov(VTMP1.Q(), Src1.Q());
      mov(VTMP2.Q(), Src2.Q());

      blr(TMP4);

      ldp<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::zr, ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI32Result();
    } break;
    case FABI_I32_V128_V128_I16: {
      // Linux Reg/Win32 Reg:
      // tmp4 (x4/x13): FallbackHandler
      // x30: return
      // vtmp1 (v0/v16): vector source 1
      // vtmp2 (v1/v17): vector source 2
      // tmp1 (x0/x10): source 1
      const auto Op = IROp->C<IR::IROp_VPCMPISTRX>();
      str<ARMEmitter::IndexType::PRE>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, -16);

      const auto Src1 = GetVReg(Op->LHS);
      const auto Src2 = GetVReg(Op->RHS);
      const auto Control = Op->Control;

      mov(VTMP1.Q(), Src1.Q());
      mov(VTMP2.Q(), Src2.Q());
      movz(ARMEmitter::Size::i32Bit, TMP1, Control);

      ldr(TMP2, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, ABIHandler));
      ldr(TMP4, FALLBACK_HANDLER_OFFSET(Info.HandlerIndex, Func));
      blr(TMP2);

      ldr<ARMEmitter::IndexType::POST>(ARMEmitter::XReg::lr, ARMEmitter::Reg::rsp, 16);
      FillI32Result();
    } break;
    case FABI_UNKNOWN:
    default:
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      LOGMAN_MSG_A_FMT("Unhandled IR Fallback ABI: {} {}", FEXCore::IR::GetName(IROp->Op), ToUnderlying(Info.ABI));
#endif
      break;
    }
  }
}

static void DirectBlockDelinker(FEXCore::Context::ExitFunctionLinkData* Record, bool Call) {
#ifdef __APPLE__
  // Self-modifying-code patch into the live MAP_JIT code buffer, invoked as an invalidation
  // callback (not through Arm64JITCore::CompileCode, so its ScopedJITWriteProtect doesn't
  // cover this) -- needs its own write-protect toggle for the same reason ExitFunctionLink
  // below does.
  FEXCore::Allocator::ScopedJITWriteProtect WriteGuard;
#endif
  uintptr_t JumpThunkStartAddress = reinterpret_cast<uintptr_t>(Record) - 0x10;
  uintptr_t CallerAddress = JumpThunkStartAddress + Record->CallerOffset;
  auto BranchOffset = JumpThunkStartAddress / 4 - CallerAddress / 4;

  // Replace the patched callsite with a branch to the jump thunk.
  uint32_t BranchInst = 0;
  ARMEmitter::Emitter BranchEmit(reinterpret_cast<uint8_t*>(&BranchInst), 4);
  if (Call) {
    BranchEmit.bl(BranchOffset);
  } else {
    BranchEmit.b(BranchOffset);
  }

  // CallerAddress was derived from Record, which is itself captured at runtime from the LR of
  // the `blr` inside the currently-*executing* jump thunk -- i.e. it's always the executable
  // (RX) address on iOS's dual-mapped scheme, which may not be writable at all. Translate back
  // to the writable (RW) alias before storing, then clear the icache at both (see JIT.cpp's
  // CompileCode ClearICache calls for why both sides need it: the write dirties a cache line
  // tagged by the RW VA, while the RX VA is the one actually fetched from).
  void* WriteCallerAddress = FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(CallerAddress));
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(WriteCallerAddress)).store(BranchInst, std::memory_order::relaxed);
  ARMEmitter::Emitter::ClearICache(WriteCallerAddress, 4);
  ARMEmitter::Emitter::ClearICache(reinterpret_cast<void*>(CallerAddress), 4);
}

static void IndirectBlockDelinker(FEXCore::Context::ExitFunctionLinkData* Record) {
#ifdef __APPLE__
  FEXCore::Allocator::ScopedJITWriteProtect WriteGuard;
#endif
  uintptr_t JumpThunkStartAddress = reinterpret_cast<uintptr_t>(Record) - 0x10;
  uint32_t BranchInst = 0;
  ARMEmitter::Emitter BranchEmit(reinterpret_cast<uint8_t*>(&BranchInst), 4);
  // Restore branch +2 instructions to jump to the linker block
  BranchEmit.b(0x2);

  // See DirectBlockDelinker above: JumpThunkStartAddress is RX-derived (from Record), needs
  // the same RW translation before writing, and both sides need clearing afterward.
  void* WriteJumpThunkStartAddress =
    FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(JumpThunkStartAddress));
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(WriteJumpThunkStartAddress)).store(BranchInst, std::memory_order::relaxed);
  ARMEmitter::Emitter::ClearICache(WriteJumpThunkStartAddress, 4);
  ARMEmitter::Emitter::ClearICache(reinterpret_cast<void*>(JumpThunkStartAddress), 4);

  // No need to reset HostCode here as the exit linker pointer is stored separately, and if the block is relinked it will be updated.
}

uint64_t Arm64JITCore::ExitFunctionLink(FEXCore::Core::CpuStateFrame* Frame, FEXCore::Context::ExitFunctionLinkData* Record) {
  auto Thread = Frame->Thread;
  bool TFSet = Thread->CurrentFrame->State.flags[X86State::RFLAG_TF_RAW_LOC];
  uintptr_t HostCode {};
  auto GuestRip = Record->GuestRIP;

  // Diagnostic-only, gated to the exact GuestRIP recorded in a 100%-reproducible Bloodborne
  // SIGILL: the host fault PC landed at Record+0xc (the GuestRIP data field 4 bytes in) with
  // Record->HostCode still 0 (this call site had never been linked before). Every static-code
  // path from here through Dispatcher.cpp's ExitFunctionLinker trampoline (register convention,
  // TMP1/TMP_ABIARGS mapping, FillStaticRegs's own scratch-register choice, the reuse-generation
  // check) has been read and checked against this build's actual config and comes back
  // consistent -- so the corruption is either in the *value* this function computes/returns, or
  // it happens in the trampoline anyway despite that reading. Logging the real return value at
  // each exit point, right where it's computed, distinguishes the two: a bad value logged here
  // means FindBlock/CompileBlock returned garbage; a good value logged here with the crash still
  // happening means the corruption is downstream of this function's return, in the assembly glue.
  constexpr uint64_t BloodborneTraceGuestRip = 0x7002047b00ULL;
  const bool TraceThisCall = GuestRip == BloodborneTraceGuestRip;
  if (TraceThisCall) {
    char trace_thread_name[64] = {};
    pthread_getname_np(pthread_self(), trace_thread_name, sizeof(trace_thread_name));
    LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: enter thread='{}' Record={:#x} Record->HostCode={:#x} "
                      "Record->CallerOffset={} TFSet={}",
                      trace_thread_name, reinterpret_cast<uintptr_t>(Record), Record->HostCode, Record->CallerOffset, TFSet);
  }

  if (TFSet) {
    // If TF is set, the cache must be skipped as different code needs to be generated.
    Frame->State.rip = GuestRip;
    if (TraceThisCall) {
      LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: return(TFSet) DispatcherLoopTop={:#x}", Frame->Pointers.DispatcherLoopTop);
    }
    return Frame->Pointers.DispatcherLoopTop;
  } else {
    {
      // Guard the LookupCache lock with the code invalidation mutex, to avoid issues with forking
      auto lk_inval =
        GuardSignalDeferringSection<std::shared_lock>(static_cast<Context::ContextImpl*>(Thread->CTX)->CodeInvalidationMutex, Thread);
      HostCode = Thread->LookupCache->FindBlock(Thread, GuestRip);
    }
    if (!HostCode) {
      // Hold a reference to the code buffer, to avoid linking unmapped code if compilation triggers a recreation.
      auto CodeBuffer = static_cast<Arm64JITCore*>(Thread->CPUBackend.get())->CurrentCodeBuffer;
      HostCode = static_cast<Context::ContextImpl*>(Thread->CTX)->CompileBlock(Frame, GuestRip, 0);
      if (Thread->LookupCache->Shared != CodeBuffer->LookupCache.get()) {
        // See the final return's own comment, below, for why this is set here too -- the
        // caller's ExitFunctionLinker (Dispatcher.cpp) branches to whatever HostCode this
        // function returns with no lock or synchronization of its own, and a safepoint-paused
        // thread caught in that window needs State.rip to already reflect this target for a
        // DispatcherLoopTop redirect to land anywhere sane. Also record the current
        // reuse-generation (see the final return's comment) for the same caller's own,
        // non-signal-dependent staleness check.
        Frame->State.rip = GuestRip;
        if (Frame->Pointers.ReuseGenerationCounterAddress) {
          Frame->ExitFunctionLinkValidatedGeneration =
            reinterpret_cast<std::atomic<uint64_t>*>(Frame->Pointers.ReuseGenerationCounterAddress)
              ->load(std::memory_order_acquire);
        }
        if (TraceThisCall) {
          LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: return(buffer-swap) HostCode={:#x} "
                            "IsAddressInCodeBuffer={}",
                            HostCode, static_cast<Context::ContextImpl*>(Thread->CTX)->IsAddressInCodeBuffer(Thread, HostCode));
        }
        return HostCode;
      }
    }
  }

  if (TraceThisCall) {
    LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: post-lookup HostCode={:#x} IsAddressInCodeBuffer={}", HostCode,
                      HostCode != 0 ? static_cast<Context::ContextImpl*>(Thread->CTX)->IsAddressInCodeBuffer(Thread, HostCode) : false);
  }

#ifdef __APPLE__
  // This function directly patches already-emitted, currently-executing JIT code in place
  // (inline call/jump-site linking) from the thread that's *running* that code -- unlike
  // Arm64JITCore::CompileCode, which is only ever reached while actively compiling and
  // already carries its own ScopedJITWriteProtect, a thread executing JIT code sits in the
  // default execute-enabled (write-disabled) MAP_JIT state, so the raw stores below need
  // their own toggle (confirmed empirically: SIGBUS on the store without it).
  FEXCore::Allocator::ScopedJITWriteProtect WriteGuard;
#endif

  // See ExitFunction in BranchOps.cpp for an assembly level view of the handled cases.
  uintptr_t JumpThunkStartAddress = reinterpret_cast<uintptr_t>(Record) - 0x10;
  uintptr_t CallerAddress = JumpThunkStartAddress + Record->CallerOffset;
  auto BranchOffset = HostCode / 4 - CallerAddress / 4;

  uint32_t ExpectedKnownCallMarkerInst = 0;
  ARMEmitter::Emitter ExpectedKnownCallMarkerEmit(reinterpret_cast<uint8_t*>(&ExpectedKnownCallMarkerInst), 4);
  ExpectedKnownCallMarkerEmit.adr(TMP1, 0xC);

  // Guard the LookupCache lock with the code invalidation mutex, to avoid issues with forking
  auto lk_inval = GuardSignalDeferringSection<std::shared_lock>(static_cast<Context::ContextImpl*>(Thread->CTX)->CodeInvalidationMutex, Thread);

  // Re-validate HostCode now that we hold CodeInvalidationMutex again -- it was captured earlier
  // (either FindBlock's fast-path hit above, or CompileBlock's return value on the slow path)
  // while that lock was held only briefly, and an iOS reuse-in-place cycle (NewCodeBuffer=false)
  // could have run entirely within the gap since then. Unlike a NewCodeBuffer=true reuse,
  // reuse-in-place clears BlockList/BlockLinks without swapping the LookupCache object itself,
  // so it's invisible to the "Thread->LookupCache->Shared != CodeBuffer->LookupCache.get()" check
  // the slow path already does above. Worse, neither that check nor the safepoint mechanism
  // protects a value already captured into this function's own HostCode local: a thread can be
  // safepoint-paused *after* reading a since-invalidated HostCode but *before* ever writing it
  // anywhere, and on resume it has no way to know the value it's still holding went stale while
  // it was frozen -- the pause only ever protected memory, not a local variable already sitting
  // in a register. Confirmed on-device as the remaining source of the exact same
  // "instruction-fetch fault, not inside any currently-live JIT allocation" crash even after the
  // safepoint's own hold window and CodeInvalidationMutex acquisition ordering were separately
  // fixed. A fresh FindBlock() here is cheap insurance (this must run before AcquireWriteLock()
  // below -- FindBlock takes its own read lock on the same underlying mutex GuestToHostMap::Lock,
  // which would deadlock against that write lock from this same thread); if it disagrees with
  // what we're still holding, re-resolve properly instead of patching a branch to a target
  // that's already gone.
  if (auto Current = Thread->LookupCache->FindBlock(Thread, GuestRip); Current != HostCode) {
    HostCode = Current ? Current : static_cast<Context::ContextImpl*>(Thread->CTX)->CompileBlock(Frame, GuestRip, 0);
    BranchOffset = HostCode / 4 - CallerAddress / 4;
  }

  // Lock here is necessary to prevent simultaneous linking and delinking
  auto lk = Thread->LookupCache->AcquireWriteLock();

  // For non-calls, this would extend into the block's code, however that's fine as an out-of-range adr would never
  // be generated avoiding any false positives.
  uintptr_t KnownCallMarkerAddr = CallerAddress - 0x8;
  uint32_t KnownCallMarkerInst = *reinterpret_cast<uint32_t*>(KnownCallMarkerAddr);
  if (ARMEmitter::Emitter::IsInt26(BranchOffset)) {
    // Directly patch the callsite with the appropriate branch instruction.
    uint32_t BranchInst = 0;
    ARMEmitter::Emitter BranchEmit(reinterpret_cast<uint8_t*>(&BranchInst), 4);

    if (KnownCallMarkerInst == ExpectedKnownCallMarkerInst) {
      BranchEmit.bl(BranchOffset);
      Thread->LookupCache->AddBlockLink(
        GuestRip, Record, [](FEXCore::Context::ExitFunctionLinkData* Record) { DirectBlockDelinker(Record, true); }, lk);
    } else {
      BranchEmit.b(BranchOffset);
      Thread->LookupCache->AddBlockLink(
        GuestRip, Record, [](FEXCore::Context::ExitFunctionLinkData* Record) { DirectBlockDelinker(Record, false); }, lk);
    }

    // CallerAddress is RX-derived (from Record, captured off the currently-executing thunk's
    // return address) -- translate to the writable alias before storing, same as
    // DirectBlockDelinker/IndirectBlockDelinker above, and clear the icache at both sides.
    void* WriteCallerAddress = FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(CallerAddress));
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(WriteCallerAddress)).store(BranchInst, std::memory_order::relaxed);
    ARMEmitter::Emitter::ClearICache(WriteCallerAddress, 4);
    ARMEmitter::Emitter::ClearICache(reinterpret_cast<void*>(CallerAddress), 4);
  } else {
    // This case is common between calls and jumps as the thunk callsite can be left untouched.
    // Record itself lives inside the JIT buffer (the ExitFunctionLinkData struct emitted right
    // in the code stream, see JIT.cpp's PendingJumpThunks loop) and is RX-derived same as
    // JumpThunkStartAddress/CallerAddress -- Record->HostCode needs the same RW translation.
    void* WriteRecord = FEXCore::Allocator::GetWritableAddress(Record);
    auto* WriteHostCode = &reinterpret_cast<FEXCore::Context::ExitFunctionLinkData*>(WriteRecord)->HostCode;
    std::atomic_ref<uint64_t>(*WriteHostCode).store(HostCode, std::memory_order::seq_cst);
#ifdef ARCHITECTURE_arm64
    // Make memory write visible to other threads reading the same location
    asm volatile("dc cvau, %0; dsb ish" : : "r"(WriteHostCode) :);
    asm volatile("dc cvau, %0; dsb ish" : : "r"(&Record->HostCode) :);
#endif

    uint32_t LdrInst = 0;
    ARMEmitter::Emitter LdrEmit(reinterpret_cast<uint8_t*>(&LdrInst), 4);
    LdrEmit.ldr(TMP1, reinterpret_cast<uint64_t>(&Record->HostCode) - JumpThunkStartAddress);
    void* WriteJumpThunkStartAddress =
      FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(JumpThunkStartAddress));
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(WriteJumpThunkStartAddress)).store(LdrInst, std::memory_order::relaxed);
    ARMEmitter::Emitter::ClearICache(WriteJumpThunkStartAddress, 4);
    ARMEmitter::Emitter::ClearICache(reinterpret_cast<void*>(JumpThunkStartAddress), 4);

    Thread->LookupCache->AddBlockLink(GuestRip, Record, IndirectBlockDelinker, lk);
  }

  // The caller (ExitFunctionLinker, Dispatcher.cpp) takes this return value in TMP1 and, after a
  // few more instructions (spilling regs, an isb, no further lock or synchronization of any
  // kind), branches straight to it -- a second, separate instance of the same class of bug the
  // HostCode re-validation above this function already fixed, just living in the assembly glue
  // around this call instead of inside it. A thread safepoint-paused anywhere in that window
  // resumes still holding this exact value with the branch to it still pending; the
  // SafepointSignalHandler (fex_guest_engine.cpp) redirects such a thread back to
  // DispatcherLoopTop, which needs State.rip to already be the real target -- match the TFSet
  // branch above, which has done this from the start for exactly this reason.
  //
  // Being paused in time isn't guaranteed, though: signal delivery has real, nonzero latency,
  // and if this whole window (from here to ExitFunctionLinker's own final branch) finishes
  // faster than that latency, this thread executes the branch having never been signaled at
  // all -- confirmed on-device as still crashing with the signal-based redirect alone in place.
  // ExitFunctionLinkValidatedGeneration is the fallback: record the reuse-generation counter's
  // value right now, at the moment HostCode is confirmed fresh, so ExitFunctionLinker can
  // re-read the live counter right before its branch and catch a reuse that completed in that
  // gap even if the signal missed it entirely.
  Frame->State.rip = GuestRip;
  if (Frame->Pointers.ReuseGenerationCounterAddress) {
    Frame->ExitFunctionLinkValidatedGeneration =
      reinterpret_cast<std::atomic<uint64_t>*>(Frame->Pointers.ReuseGenerationCounterAddress)
        ->load(std::memory_order_acquire);
  }
  // Diagnostic-only, gated to one known-deterministic guest RIP: chasing a 100%-reproducible
  // "instruction-fetch fault, not inside any currently-live JIT allocation" crash that always
  // resolves this exact GuestRIP. The reuse-generation check above (Dispatcher.cpp's
  // ExitFunctionLinker) already confirmed no buffer-reuse cycle happens around the fault --
  // live_gen matched ExitFunctionLinkValidatedGeneration exactly -- so the HostCode this
  // function is about to return is either wrong from the moment it was computed (a genuine
  // CompileBlock/FindBlock correctness bug for this specific guest function) or gets corrupted
  // somewhere between this return and ExitFunctionLinker's own branch, outside the reuse
  // mechanism entirely. Logging the exact value right here, at the one point closest to its
  // computation, settles which.
  if (GuestRip == 0x70007b9cf0ULL) {
    LogMan::Msg::EFmt("BACHATA_EXITFUNCTIONLINK_TARGET: GuestRip={:#x} HostCode={:#x} TFSet={} "
                      "IsAddressInCodeBuffer={}",
                      GuestRip, HostCode, TFSet,
                      HostCode != 0 ? static_cast<Context::ContextImpl*>(Thread->CTX)->IsAddressInCodeBuffer(Thread, HostCode) : false);
  }
  return HostCode;
}

void Arm64JITCore::Op_NoOp(const IR::IROp_Header* IROp, IR::Ref Node) {}

Arm64JITCore::Arm64JITCore(FEXCore::Context::ContextImpl* ctx, FEXCore::Core::InternalThreadState* Thread)
  : CPUBackend(*ctx, Thread)
  , Arm64Emitter(ctx)
  , HostSupportsSVE128 {ctx->HostFeatures.SupportsSVE128}
  , HostSupportsSVE256 {ctx->HostFeatures.SupportsSVE256}
  , HostSupportsAVX256 {ctx->HostFeatures.SupportsAVX && ctx->HostFeatures.SupportsSVE256}
  , HostSupportsRPRES {ctx->HostFeatures.SupportsRPRES}
  , HostSupportsAFP {ctx->HostFeatures.SupportsAFP}
  , CTX {ctx}
  , TempAllocator(ctx->CPUBackendAllocator, 0) {

  RAPass = Thread->PassManager->GetPass<IR::RegisterAllocationPass>("RA");

  RAPass->AddRegisters(IR::RegClass::GPR, GeneralRegisters.size());
  RAPass->AddRegisters(IR::RegClass::GPRFixed, StaticRegisters.size());
  RAPass->AddRegisters(IR::RegClass::FPR, GeneralFPRegisters.size());
  RAPass->AddRegisters(IR::RegClass::FPRFixed, StaticFPRegisters.size());
  RAPass->PairRegs = PairRegisters;

  {
    // Set up pointers that the JIT needs to load

    // Common
    auto& Ptrs = ThreadState->CurrentFrame->Pointers;

    Ptrs.PrintValue = reinterpret_cast<uint64_t>(PrintValue);
    Ptrs.PrintVectorValue = reinterpret_cast<uint64_t>(PrintVectorValue);
    Ptrs.PrintMsgValue = reinterpret_cast<uint64_t>(PrintMsg);

    Ptrs.ThreadRemoveCodeEntryFromJIT = reinterpret_cast<uintptr_t>(&Context::ContextImpl::ThreadRemoveCodeEntryFromJit);
    Ptrs.MonoBackpatcherWrite = reinterpret_cast<uint64_t>(&Context::ContextImpl::MonoBackpatcherWrite);
    Ptrs.CPUIDObj = reinterpret_cast<uint64_t>(&CTX->CPUID);

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::CPUIDEmu::RunFunction);
      Ptrs.CPUIDFunction = PMF.GetConvertedPointer();
    }

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::CPUIDEmu::RunXCRFunction);
      Ptrs.XCRFunction = PMF.GetConvertedPointer();
    }

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::HLE::SyscallHandler::HandleSyscall);
      Ptrs.SyscallHandlerObj = reinterpret_cast<uint64_t>(CTX->SyscallHandler);
      Ptrs.SyscallHandlerFunc = PMF.GetVTableEntry(CTX->SyscallHandler);
    }
    Ptrs.ExitFunctionLink = reinterpret_cast<uintptr_t>(&Arm64JITCore::ExitFunctionLink);
    Ptrs.LUDIV = reinterpret_cast<uint64_t>(LUDIV);
    Ptrs.LDIV = reinterpret_cast<uint64_t>(LDIV);
  }

  CurrentCodeBuffer = CodeBuffers.GetLatest();
  ThreadState->LookupCache->Shared = CurrentCodeBuffer->LookupCache.get();
}

void Arm64JITCore::EmitDetectionString() {
  const char JITString[] = "FEXJIT::Arm64JITCore::";
  EmitString(JITString);
  Align();
}

void Arm64JITCore::ClearCache() {
  // NOTE: Holding on to the reference here is required to ensure validity of the WriteLock mutex
  auto PrevCodeBuffer = CurrentCodeBuffer;
  auto lk = PrevCodeBuffer->LookupCache->AcquireWriteLock();

  // Delink direct branches other already-compiled code patched straight into blocks living in
  // PrevCodeBuffer -- ChangeGuestToHostMapping below only clears this thread's own L1/L2 and
  // swaps Shared to the new buffer, it never touches PrevCodeBuffer's own BlockLinks. Once
  // PrevCodeBuffer's memory is eventually reclaimed/reused (this build's iOS JIT allocations
  // are a small, StikDebug-count-limited shared pool -- see ClearCodeCache's NewCodeBuffer=false
  // path, Core.cpp), any surviving direct patch into it becomes a landmine: a jump to memory
  // that's since gone from "stale content" to "not part of any live allocation at all".
  // Confirmed on-device: a crash with exactly that signature (LR inside the dispatcher's
  // ExitFunctionLinker trampoline, PC entirely outside every current buffer) persisted even
  // after the reuse-in-place path's own cross-thread cache sync was fully fixed and verified
  // working -- this fresh-allocation path was the one remaining place still skipping delinking.
  PrevCodeBuffer->LookupCache->ClearCache(lk);

  auto CodeBuffer = GetEmptyCodeBuffer();
  SetBuffer(CodeBuffer->Ptr, CodeBuffer->AllocatedSize);
  EmitDetectionString();

  ThreadState->LookupCache->ChangeGuestToHostMapping(*PrevCodeBuffer, *CurrentCodeBuffer->LookupCache, lk);
  // Sibling guest threads' independent L1/L2 caches also need to be reached here, same as the
  // reuse-in-place path -- this fresh allocation just as validly invalidates their stale
  // entries as an in-place reuse does. See OnBufferReusedInPlace's own comment (Context.h).
  if (CTX->OnBufferReusedInPlace) {
    CTX->OnBufferReusedInPlace(ThreadState, lk);
  }
}

Arm64JITCore::~Arm64JITCore() {}

bool Arm64JITCore::IsInlineConstant(const IR::OrderedNodeWrapper& WNode, uint64_t* Value) const {
  if (WNode.IsImmediate()) {
    return false;
  }

  auto OpHeader = IR->GetOp<IR::IROp_Header>(WNode);

  if (OpHeader->Op == IR::IROps::OP_INLINECONSTANT) {
    auto Op = OpHeader->C<IR::IROp_InlineConstant>();
    if (Value) {
      *Value = Op->Constant;
    }
    return true;
  } else {
    return false;
  }
}

bool Arm64JITCore::IsInlineEntrypointOffset(const IR::OrderedNodeWrapper& WNode, uint64_t* Value) const {
  if (WNode.IsImmediate()) {
    return false;
  }

  auto OpHeader = IR->GetOp<IR::IROp_Header>(WNode);

  if (OpHeader->Op == IR::IROps::OP_INLINEENTRYPOINTOFFSET) {
    auto Op = OpHeader->C<IR::IROp_InlineEntrypointOffset>();
    if (Value) {
      uint64_t Mask = ~0ULL;
      const auto Size = OpHeader->Size;
      if (Size == IR::OpSize::i32Bit) {
        Mask = 0xFFFF'FFFFULL;
      }
      *Value = (Entry + Op->Offset) & Mask;
    }
    return true;
  } else {
    return false;
  }
}

void Arm64JITCore::EmitTFCheck() {
  ARMEmitter::ForwardLabel l_TFUnset;
  ARMEmitter::ForwardLabel l_TFBlocked;

  // Note that this needs to be before the below suspend checks, as X86 checks this flag immediately after executing an instruction.
  ldrb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));

  (void)cbz(ARMEmitter::Size::i32Bit, TMP1, &l_TFUnset);

  // X86 semantically checks TF after executing each instruction, so e.g. setting a context with TF set will execute a single instruction
  // and then raise an exception. However on the FEX side this is simpler to implement by checking at the start of each instruction, handle this by having bit 1 being unset in the flag state indicate that TF is blocked for a single instruction.
  (void)tbz(TMP1, 1, &l_TFBlocked);

  // Block TF for a single instruction when the frontend jumps to a new context by unsetting bit 1.
  ldrb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));
  and_(ARMEmitter::Size::i32Bit, TMP1, TMP1, ~(1 << 1));
  strb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));

  Core::CpuStateFrame::SynchronousFaultDataStruct State = {
    .FaultToTopAndGeneratedException = 1,
    .Signal = Core::FAULT_SIGTRAP,
    .TrapNo = X86State::X86_TRAPNO_DB,
    .si_code = 2,
    .err_code = 0,
  };

  uint64_t Constant {};
  memcpy(&Constant, &State, sizeof(State));

  LoadConstant(ARMEmitter::Size::i64Bit, TMP1, Constant);
  str(TMP1, STATE, offsetof(FEXCore::Core::CpuStateFrame, SynchronousFaultData));
  ldr(TMP1, STATE, offsetof(FEXCore::Core::CpuStateFrame, Pointers.GuestSignal_SIGTRAP));
  br(TMP1);

  (void)Bind(&l_TFBlocked);
  // If TF was blocked for this instruction, unblock it for the next.
  LoadConstant(ARMEmitter::Size::i32Bit, TMP1, 0b11);
  strb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));
  (void)Bind(&l_TFUnset);
}

void Arm64JITCore::EmitSuspendInterruptCheck() {
  if (CTX->Config.NeedsPendingInterruptFaultCheck) {
    // Trigger a fault if there are any pending interrupts
    // Used only for suspend on WIN32 at the moment
    constexpr size_t InterruptPageOffset =
      offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) - offsetof(FEXCore::Core::InternalThreadState, BaseFrameState);
    if constexpr (InterruptPageOffset <= 32760) {
      str(ARMEmitter::XReg::zr, STATE, InterruptPageOffset);
    } else {
      // Need to use vector 128-bit store for this range.
      // Doesn't matter which register we use to store.
      str(ARMEmitter::QReg::q0, STATE, InterruptPageOffset);
    }
  }

#ifdef ARCHITECTURE_arm64ec
  static constexpr uint16_t SuspendMagic {0xCAFE};

  ldr(TMP2.W(), STATE_PTR(CpuStateFrame, SuspendDoorbell));
  ARMEmitter::ForwardLabel l_NoSuspend;
  (void)cbz(ARMEmitter::Size::i32Bit, TMP2, &l_NoSuspend);
  brk(SuspendMagic);
  (void)Bind(&l_NoSuspend);
#endif
}

void Arm64JITCore::EmitEntryPoint(ARMEmitter::BackwardLabel& HeaderLabel, bool CheckTF) {
  // Get the address of the JITCodeHeader and store in to the core state.
  // Two instruction cost, each 1 cycle.
  adr_OrRestart(TMP1, &HeaderLabel);
  str(TMP1, STATE, offsetof(FEXCore::Core::CPUState, InlineJITBlockHeader));

  if (CheckTF) {
    EmitTFCheck();
  }

  if (SpillSlots) {
    const auto TotalSpillSlotsSize = SpillSlots * MaxSpillSlotSize;

    if (ARMEmitter::IsImmAddSub(TotalSpillSlotsSize)) {
      sub(ARMEmitter::Size::i64Bit, ARMEmitter::Reg::rsp, ARMEmitter::Reg::rsp, TotalSpillSlotsSize);
    } else {
      LoadConstant(ARMEmitter::Size::i64Bit, TMP1, TotalSpillSlotsSize);
      sub(ARMEmitter::Size::i64Bit, ARMEmitter::XReg::rsp, ARMEmitter::XReg::rsp, TMP1, ARMEmitter::ExtendedType::LSL_64, 0);
    }
  }

  EmitSuspendInterruptCheck();
}

CPUBackend::CompiledCode Arm64JITCore::CompileCode(uint64_t Entry, uint64_t Size, bool SingleInst, const FEXCore::IR::IRListView* IR,
                                                   FEXCore::Core::DebugData* DebugData, bool CheckTF) {
  FEXCORE_PROFILE_SCOPED("Arm64::CompileCode");

#ifdef __APPLE__
  // Constructed before the UncheckedLongJump::SetJump restart checkpoint below on purpose --
  // see ScopedJITWriteProtect's comment. Flips this thread back to execute-enabled on any
  // real return from this function, whether this attempt succeeds or a restart loop below
  // eventually does.
  FEXCore::Allocator::ScopedJITWriteProtect WriteGuard;
#endif

  const auto PrevNumAllocations = Relocations.size();

  this->Entry = Entry;
  this->DebugData = DebugData;
  this->IR = IR;
  RequiresFarARM64Jumps = false;
  SSANodeMultiplier = 24;

  // Prepare restart via long jump in case branch encoding fails.
  // This uses UncheckedLongJump since we don't implement std::longjmp in WoA setups
  switch (static_cast<RestartOptions::Control>(FEXCore::UncheckedLongJump::SetJump(ThreadState->RestartJump))) {
  case RestartOptions::Control::Incoming:
    // Nothing
    break;
  case RestartOptions::Control::EnableFarARM64Jumps: RequiresFarARM64Jumps = true; break;
  case RestartOptions::Control::NeedsLargerJITSpace:
    // Get rid of the claimed buffer immediately, we can't fit in it at all.
    TempAllocator.UnclaimBuffer();
    SSANodeMultiplier *= 2;
    break;
  default: LOGMAN_MSG_A_FMT("Unhandled Arm64 restart condition!");
  }

  uint32_t SSACount = IR->GetSSACount();
  JumpTargets.clear();
  CallReturnTargets.clear();
  PendingJumpThunks.clear();
  JumpTargets.resize(IR->GetHeader()->BlockCount, {});
  Relocations.resize(PrevNumAllocations, FEXCore::CPU::Relocation::Default()); // Discard any relocations generated from a previous attempt

  CodeData.EntryPoints.clear();

  // Fairly excessive buffer range to make sure we don't overflow
  // One page baseline, plus SSANodeMultipler bytes, plus another page for guard page.
  const uint32_t DesiredBufferRange = AlignUp(FEXCore::Utils::FEX_PAGE_SIZE * 2 + SSACount * SSANodeMultiplier, FEXCore::Utils::FEX_PAGE_SIZE);

  // JIT output is first written to a temporary buffer and later relocated to the CodeBuffer.
  // This minimizes lock contention of CodeBufferWriteMutex.
  auto TempCodeBufferInfo = TempAllocator.ReownOrClaimBufferWithSize(DesiredBufferRange);
  auto TempCodeBuffer = TempCodeBufferInfo.Ptr;
  const uint32_t UsableBufferRange = TempCodeBufferInfo.Size - FEXCore::Utils::FEX_PAGE_SIZE;

  SetBuffer(TempCodeBuffer, UsableBufferRange);

  ThreadState->JITGuardPage = reinterpret_cast<uintptr_t>(TempCodeBuffer) + UsableBufferRange;
  ThreadState->JITGuardOverflowArgument = FEXCore::ToUnderlying(RestartOptions::Control::NeedsLargerJITSpace);

  CodeData.BlockBegin = GetCursorAddress<uint8_t*>();

  // Put the code header at the start of the data block.
  ARMEmitter::BackwardLabel JITCodeHeaderLabel {};
  (void)Bind(&JITCodeHeaderLabel);
  JITCodeHeader* CodeHeader = GetCursorAddress<JITCodeHeader*>();
  CursorIncrement(sizeof(JITCodeHeader));

  auto CodeBegin = GetCursorAddress<uint8_t*>();

  // AAPCS64
  // r30      = LR
  // r29      = FP
  // r19..r28 = Callee saved
  // r18      = Platform Register (Matters if we target Windows or iOS)
  // r16..r17 = Inter-procedure scratch
  //  r9..r15 = Temp
  //  r8      = Indirect Result
  //  r0...r7 = Parameter/Results
  //
  //  FPRS:
  //  v8..v15 = (lower 64bits) Callee saved

  // Our allocation:
  // X0 = ThreadState
  // X1 = MemBase
  //
  // X1-X3 = Temp
  // X4-r18 = RA

  SpillSlots = IR->SpillSlots();

  PendingTargetLabel = nullptr;
  PendingCallReturnTargetLabel = nullptr;

  for (auto [BlockNode, BlockHeader] : IR->GetBlocks()) {
    auto BlockIROp = BlockHeader->CW<FEXCore::IR::IROp_CodeBlock>();
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    LOGMAN_THROW_A_FMT(BlockIROp->Header.Op == IR::OP_CODEBLOCK, "IR type failed to be a code block");
#endif

    auto BlockStartHostCode = GetCursorAddress<uint8_t*>();
    {
      const auto Node = IR->GetID(BlockNode);
      const auto Target = &JumpTargets[BlockIROp->ID];

      // if there's a pending branch, and it is not fall-through
      if (PendingTargetLabel && PendingTargetLabel != Target) {
        if (PendingTargetLabel->Backward.Location) {
          EmitSuspendInterruptCheck();
        }
        b_OrRestart(PendingTargetLabel);
        PendingTargetLabel = nullptr;
      }

      if (BlockIROp->EntryPoint) {
        uint64_t BlockStartRIP = Entry + BlockIROp->GuestEntryOffset;

        const auto IsReturnTarget = CallReturnTargets.try_emplace(Node).first;
        if (PendingTargetLabel) {
          // If there is a fallthrough branch to this block, skip over the entrypoint code.
          b_OrRestart(Target);
        } else if (PendingCallReturnTargetLabel && PendingCallReturnTargetLabel != &IsReturnTarget->second) {
          // If we just emitted a call, but the block we're now emitting is not the return block so don't fallthrough.
          b_OrRestart(PendingCallReturnTargetLabel);
        }
        PendingCallReturnTargetLabel = nullptr;

        BindOrRestart(&IsReturnTarget->second);
        CodeData.EntryPoints.emplace(BlockStartRIP, GetCursorAddress<uint8_t*>());
        DebugData->GuestOpcodes.push_back({BlockIROp->GuestEntryOffset, GetCursorAddress<uint8_t*>() - CodeData.BlockBegin});

        EmitEntryPoint(JITCodeHeaderLabel, CheckTF);
      }

      if (PendingCallReturnTargetLabel) {
        // If there is still a pending call return target, then the block we're emitting is not the return block so don't fallthrough.
        b_OrRestart(PendingCallReturnTargetLabel);
        PendingCallReturnTargetLabel = nullptr;
      }
      PendingTargetLabel = nullptr;

      BindOrRestart(Target);
    }

    for (auto [CodeNode, IROp] : IR->GetCode(BlockNode)) {
      switch (IROp->Op) {
#define REGISTER_OP(op, x) \
  case FEXCore::IR::IROps::OP_##op: Op_##x(IROp, CodeNode); break

#define IROP_DISPATCH_DISPATCH
#include <FEXCore/IR/IRDefines_Dispatch.inc>
#undef REGISTER_OP

      default: Op_Unhandled(IROp, CodeNode); break;
      }
    }

    DebugData->Subblocks.push_back({static_cast<uint32_t>(BlockStartHostCode - CodeData.BlockBegin),
                                    static_cast<uint32_t>(GetCursorAddress<uint8_t*>() - BlockStartHostCode)});
  }

  // Make sure last branch is generated. It certainly can't be eliminated here.
  if (PendingTargetLabel) {
    if (PendingTargetLabel->Backward.Location) {
      EmitSuspendInterruptCheck();
    }
    b_OrRestart(PendingTargetLabel);
  }
  PendingTargetLabel = nullptr;

  // Diagnostic-only: collects the thunk-start address (and its offset-0x08 ldr instruction's own
  // address) for every PendingJumpThunk targeting Bloodborne's traced GuestRIP compiled into this
  // block, so their actual emitted bytes can be re-read and logged once the trailing loop's own
  // shared l_ExitLink label (reused across every iteration below -- see AddLocationToLabel/Bind
  // in CodeEmitter/Emitter.h, which never clears FirstInst/Insts between binds) gets its final,
  // correct rebind to the real shared-linker symbol after the loop. Traced statically through
  // Bind's own logic, every intermediate rebind inside the loop *should* get overwritten by that
  // final one -- this dump is here to confirm that holds for real compiled output, since a
  // Bloodborne SIGILL traced back to this same GuestRIP lands inside a *different*, never-linked
  // thunk that never even entered ExitFunctionLink, which the static trace alone couldn't explain.
  struct BloodborneTracedThunk {
    uint64_t ThunkAddress;
    uint64_t CallerAddress;
  };
  fextl::vector<BloodborneTracedThunk> BloodborneTracedThunks;
  constexpr uint64_t BloodborneTraceGuestRip = 0x7002047b00ULL;

  ARMEmitter::ForwardLabel l_ExitLink;
  for (auto& PendingJumpThunk : PendingJumpThunks) {
    // Align as 64-bit atomics are used on the HostCode field.
    Align(8);

    ARMEmitter::ForwardLabel l_DoLink;
    uint64_t ThunkAddress = GetCursorAddress<uint64_t>();
    BindOrRestart(&PendingJumpThunk.Label);
    b_OrRestart(&l_DoLink);
    br(TMP1);
    BindOrRestart(&l_DoLink);
    ldr(TMP1, &l_ExitLink);
    blr(TMP1);

    // This is a ExitFunctionLinkData struct
    BindOrRestart(&l_ExitLink);
    dc64(0);                                                                   // HostCode
    PlaceNamedSymbolLiteral(InsertGuestRIPLiteral(PendingJumpThunk.GuestRIP)); // GuestRIP
    dc64(PendingJumpThunk.CallerAddress - ThunkAddress);                       // CallerOffset

    if (PendingJumpThunk.GuestRIP == BloodborneTraceGuestRip) {
      BloodborneTracedThunks.push_back({ThunkAddress, PendingJumpThunk.CallerAddress});
    }
  }

  BindOrRestart(&l_ExitLink);
  PlaceNamedSymbolLiteral(InsertNamedSymbolLiteral(RelocNamedSymbolLiteral::NamedSymbol::SYMBOL_LITERAL_EXITFUNCTION_LINKER));

  // See BloodborneTracedThunks' own comment above, at this loop's declaration. Reading these back
  // now, right after l_ExitLink's final rebind, shows whether every thunk's offset-0x08 ldr and
  // offset-0x00 unlinked-entry instruction actually ended up correct at the moment this compile
  // finishes -- if they're already wrong here, the bug is in this emission/labeling code; if
  // they're correct here but Bloodborne still crashes on a later run, the corruption happens
  // afterward (buffer reuse, a later compile overlapping this memory, or a runtime patch race).
  for (const auto& Traced : BloodborneTracedThunks) {
    const uint32_t Word00 = *reinterpret_cast<const uint32_t*>(Traced.ThunkAddress + 0x00);
    const uint32_t Word04 = *reinterpret_cast<const uint32_t*>(Traced.ThunkAddress + 0x04);
    const uint32_t Word08 = *reinterpret_cast<const uint32_t*>(Traced.ThunkAddress + 0x08);
    const uint32_t Word0c = *reinterpret_cast<const uint32_t*>(Traced.ThunkAddress + 0x0c);
    const uint64_t HostCodeField = *reinterpret_cast<const uint64_t*>(Traced.ThunkAddress + 0x10);
    const uint32_t CallerWord = *reinterpret_cast<const uint32_t*>(Traced.CallerAddress);
    LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: post-compile thunk ThunkAddress={:#x} "
                      "CallerAddress={:#x} CallerWord={:#x} word00={:#x} word04={:#x} word08={:#x} "
                      "word0c={:#x} HostCodeField={:#x}",
                      Traced.ThunkAddress, Traced.CallerAddress, CallerWord, Word00, Word04, Word08, Word0c,
                      HostCodeField);
  }

  // CodeSize not including the header or tail data.
  const uint64_t CodeOnlySize = GetCursorAddress<uint8_t*>() - CodeBegin;

  // Add the JitCodeTail (written later)
  Align(alignof(JITCodeTail));
  const auto JITBlockTailLocation = GetCursorAddress<uint8_t*>();
  CodeHeader->OffsetToBlockTail = JITBlockTailLocation - CodeData.BlockBegin;

  JITCodeTail JITBlockTail {
    .RIP = Entry,
    .GuestSize = Size,
    .SpinLockFutex = 0,
    .SingleInst = SingleInst,
  };

  // Entries that live after the JITCodeTail.
  // These entries correlate JIT code regions with guest RIP regions.
  // Using these entries FEX is able to reconstruct the guest RIP accurately when an instruction cause a signal fault.
  // Packed using two variable length integer entries to ensure the size isn't too large.
  // These smaller sizes means that each entry is relative to each other instead of absolute offset from the start of the JIT block.
  // When reconstructing the RIP, each entry must be walked linearly and accumulated with the previous entries.
  // This is a trade-off between compression inside the JIT code space and execution time when reconstruction the RIP.
  // RIP reconstruction when faulting is less likely so we are requiring the accumulation.
  //
  // struct {
  //   // The Host PC offset from the previous entry.
  //   FEXCore::Utils::vl64 HostPCOffset;
  //   // How much to offset the RIP from the previous entry.
  //   FEXCore::Utils::vl64 GuestRIPOffset;
  // };

  const auto JITRIPEntriesBegin = JITBlockTailLocation + sizeof(JITBlockTail);
  auto JITRIPEntriesLocation = JITRIPEntriesBegin;

  {
    // Store the RIP entries.
    JITBlockTail.NumberOfRIPEntries = DebugData->GuestOpcodes.size();
    JITBlockTail.OffsetToRIPEntries = JITRIPEntriesBegin - JITBlockTailLocation;
    uintptr_t CurrentRIPOffset = 0;
    uint64_t CurrentPCOffset = 0;

    for (size_t i = 0; i < DebugData->GuestOpcodes.size(); i++) {
      const auto& GuestOpcode = DebugData->GuestOpcodes[i];
      int64_t HostPCOffset = GuestOpcode.HostEntryOffset - CurrentPCOffset;
      int64_t GuestRIPOffset = GuestOpcode.GuestEntryOffset - CurrentRIPOffset;

      JITRIPEntriesLocation += FEXCore::Utils::vl64pair::Encode(JITRIPEntriesLocation, HostPCOffset, GuestRIPOffset);

      CurrentPCOffset = GuestOpcode.HostEntryOffset;
      CurrentRIPOffset = GuestOpcode.GuestEntryOffset;
    }
  }

  SetCursorOffset(JITRIPEntriesLocation - CodeData.BlockBegin);
  Align();

  CodeData.Size = GetCursorAddress<uint8_t*>() - CodeData.BlockBegin;

  // Finalize and write block tail data
  JITBlockTail.Size = CodeData.Size;
  {
    auto PrevCur = GetCursorOffset();
    memcpy(JITBlockTailLocation, &JITBlockTail, sizeof(JITBlockTail));
    SetCursorOffset(JITBlockTailLocation - CodeData.BlockBegin + offsetof(JITCodeTail, RIP));
    PlaceNamedSymbolLiteral(InsertGuestRIPLiteral(JITBlockTail.RIP));
    SetCursorOffset(PrevCur);
  }

  // Migrate the compile output from temporary storage to the actual CodeBuffer.
  // This can block progress in other compiling threads, so the duration of the lock should be as small as possible.
  {
    auto CodeBufferLock = std::unique_lock {CodeBuffers.CodeBufferWriteMutex};

    // Set below, on the iOS reuse-in-place path only, when this compile actually took
    // CodeInvalidationMutex exclusively and opened a BeginBufferInvalidationSafepoint pause --
    // see the matching unlock()/EndBufferInvalidationSafepoint call near this function's memcpy
    // below, and BeginBufferInvalidationSafepoint's own comment (Context.h) for why both need to
    // stay held that far past the metadata clear, and why the mutex must be acquired before the
    // pause is opened rather than after.
    bool HeldReuseSafepoint = false;

    // Query size of generated code
    const auto TempSize = GetCursorOffset();

    // Bring CodeBuffer up to date
    {
      LOGMAN_THROW_A_FMT(CurrentCodeBuffer->LookupCache.get() == ThreadState->LookupCache->Shared, "INVARIANT VIOLATED: SharedLookupCache "
                                                                                                   "doesn't match up!\n");
      if (auto Prev = CheckCodeBufferUpdate()) {
        Allocator::VirtualDontNeed(ThreadState->CallRetStackBase, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE);
        auto lk = ThreadState->LookupCache->AcquireWriteLock();
        ThreadState->LookupCache->ChangeGuestToHostMapping(*Prev, *CurrentCodeBuffer->LookupCache, lk);
      }

      // NOTE: 16-byte alignment of the new cursor offset must be preserved for block linking records
      SetBuffer(CurrentCodeBuffer->Ptr, CurrentCodeBuffer->AllocatedSize);
      SetCursorOffset(CodeBuffers.LatestOffset);
      Align16B();
      if ((GetCursorOffset() + TempSize) > CurrentCodeBuffer->UsableSize()) {
#if defined(FEX_APPLE_DUAL_MAPPED_JIT)
        // NewCodeBuffer=true (the default) requests a brand new dual-mapped allocation from
        // StikDebug's external BreakpointJIT mechanism every time the cache fills, which fails
        // on a session's 3rd+ such request (confirmed on-device, independent of the requested
        // size -- see CPUBackend.cpp's iOS MAX_CODE_SIZE comment). NewCodeBuffer=false reuses
        // the same already-granted buffer in place instead, avoiding that failure entirely.
        // This was tried once before and reverted: GuestToHostMap::ClearCache
        // (LookupCache.cpp) discarded its BlockLinks map without ever calling each entry's
        // delinker, so already-patched direct branches in other, non-evicted code kept
        // pointing at addresses inside the reused region -- once that physical memory got
        // overwritten with new compiled code, re-taking one of those stale links landed on
        // unrelated bytes, and the resulting fault's own recovery path could only fix the
        // register for one instruction, not the stale cached target, so the same handful of
        // addresses re-faulted forever (confirmed on-device: 7,000,000+ recoveries across
        // only 8 distinct addresses, no crash, no progress). ClearCache now properly severs
        // every link before discarding them, the same way Erase() already did for a single
        // address, which is the actual fix -- re-enabling reuse-in-place here now that the
        // real bug underneath it is fixed, rather than working around it by avoiding this
        // path altogether.
        //
        // ClearCodeCache's NewCodeBuffer=false branch (Core.cpp) requires the caller to already
        // hold CodeInvalidationMutex exclusively before calling in, to exclude any other thread
        // that might currently be *executing* out of the buffer being reused (not just compiling
        // into it -- execution of already-JIT'd code never touches this mutex at all, so this is
        // the only guard available). This call site is reached from deep inside CompileBlock
        // (Core.cpp), which already holds that same mutex *shared* for its entire duration
        // (GuardSignalDeferringSection<std::shared_lock>) -- this mutex doesn't support
        // recursive or shared-to-exclusive-upgrade locking, so acquiring the exclusive lock
        // without first releasing that shared reference would deadlock against ourselves.
        // Release it manually here and restore it (see below, past the memcpy), bypassing the
        // RAII wrapper CompileBlock's local `lk` owns (its own eventual unlock_shared() in that
        // caller's destructor stays correctly balanced as long as this thread holds exactly one
        // shared reference again by the time this function returns, which the sequence below
        // guarantees) -- WritePriorityMutex::Mutex doesn't track *which* call site (de)acquired
        // it, only the net reader/writer counts, so this is safe purely at the mutex level.
        //
        // Acquire the exclusive lock BEFORE opening the safepoint pause, not after -- see
        // BeginBufferInvalidationSafepoint's own comment (Context.h) for the deadlock this
        // ordering avoids: pausing every other thread first can freeze one of them mid-hold of
        // this same mutex (shared, from inside its own CompileBlock/ExitFunctionLink), and it can
        // never release that hold until its pause lifts, which can't happen until we finish here
        // -- confirmed on-device as an indefinite hang with the safepoint itself reporting every
        // other thread successfully paused. Taking the exclusive lock first sidesteps this
        // entirely: once acquired, no thread can currently hold it shared, so freezing everyone
        // afterward can't catch one mid-hold.
        //
        // The pause itself (BeginBufferInvalidationSafepoint/EndBufferInvalidationSafepoint) is
        // owned here, not inside ClearCodeCache, because clearing the LookupCache metadata alone
        // doesn't make the buffer safe to resume into -- the bytes a paused-then-resumed thread
        // was already executing stay physically unchanged until this function's memcpy below
        // actually overwrites them. Releasing the pause right after the metadata clear (as
        // ClearCodeCache used to do on its own) leaves a window where a just-resumed thread can
        // race that memcpy into the exact memory it's still running through -- confirmed on-device
        // as two different guest threads instruction-fetch-faulting at addresses only 0x80 apart,
        // both landing in memory the LookupCache had already forgotten. Held open from here
        // through the memcpy closes that window; see the matching End call below.
        auto& InvalidationMutex = CTX->GetCodeInvalidationMutex();
        InvalidationMutex.unlock_shared();
        InvalidationMutex.lock();
        if (CTX->BeginBufferInvalidationSafepoint) {
          CTX->BeginBufferInvalidationSafepoint(ThreadState);
          HeldReuseSafepoint = true;
        }
        CTX->ClearCodeCache(ThreadState, false);
        // ClearCodeCache(..., false) only clears the LookupCache's metadata (BlockList/
        // BlockLinks) -- it never touches this Emitter's own write cursor, which is still
        // sitting wherever it was when the overflow check above fired (at/near the buffer's
        // usable end -- that's *why* the overflow fired). Left alone, this compile's memcpy
        // below writes starting from that same near-end offset, past the buffer's actual
        // bounds, and CodeBuffers.LatestOffset just below records that same out-of-bounds
        // position as the new "latest" -- so every subsequent compile re-triggers this branch
        // and drifts the offset even further past the allocation with each cycle. Downstream,
        // the resulting EntryPoint (rebased via Delta below onto this already-out-of-range
        // cursor) sits outside the buffer's tracked RW range, so GetExecutableAddress's table
        // lookup (AllocatorHooks.h) falls back to returning it untranslated -- an RW-side value
        // masquerading as the cached HostCode, which then fails every IsAddressInCodeBuffer
        // check downstream. This is the root cause behind the "not inside any currently-live
        // JIT allocation" crash chased across this whole investigation. SetBuffer resets the
        // cursor to the buffer's start, matching what the NewCodeBuffer=true path already does
        // via its own SetBuffer call inside Arm64JITCore::ClearCache() after allocating fresh.
        SetBuffer(CurrentCodeBuffer->Ptr, CurrentCodeBuffer->AllocatedSize);
#else
        CTX->ClearCodeCache(ThreadState);
#endif
      }

      CodeBuffers.LatestOffset = GetCursorOffset();
    }

    // Adjust host addresses
    const auto Delta = GetCursorAddress<uint8_t*>() - CodeData.BlockBegin;
    CodeData.BlockBegin += Delta;
    for (auto& EntryPoint : CodeData.EntryPoints) {
      EntryPoint.second += Delta;
      // EntryPoints' values escape this function (see ContextImpl::CompileBlock in Core.cpp,
      // which returns one directly as the uintptr_t the dispatcher branches to via
      // "Ptr = CTX->CompileBlock(RIP); Ptr();", and they're inserted into the LookupCache other
      // JIT'd code reads and branches to indirectly) -- unlike BlockBegin below, which is only
      // ever used for symbol/debug bookkeeping, never branched to, so it stays a write address.
      auto* const WriteSideValue = EntryPoint.second;
      EntryPoint.second = reinterpret_cast<uint8_t*>(FEXCore::Allocator::GetExecutableAddress(EntryPoint.second));
      // Diagnostic-only, gated to the same GuestRIP as ExitFunctionLink's BACHATA_BLOODBORNE_
      // THUNK_TRACE (this function's `Entry` is that same GuestRIP for the block being compiled
      // right now). GetExecutableAddress (Allocator.cpp) returns its input unchanged whenever the
      // write-side address isn't inside any region this thread's JIT allocator has actually
      // registered -- an already-confirmed root cause (see this file's ClearCodeCache/SetBuffer
      // comment above, ~line 1373) for a write-side address masquerading as a valid HostCode,
      // landing wherever that raw number happens to be executable-mapped once branched to. If
      // this fires unchanged for Bloodborne's traced GuestRIP, this is the same bug through a
      // different trigger than the one already fixed there.
      if (Entry == 0x7002047b00ULL && EntryPoint.second == WriteSideValue) {
        LogMan::Msg::EFmt("BACHATA_BLOODBORNE_THUNK_TRACE: CompileCode GuestRIP={:#x} WriteSideValue={:#x} "
                          "GetExecutableAddress returned it UNCHANGED (untranslated -- likely bug)",
                          Entry, reinterpret_cast<uintptr_t>(WriteSideValue));
      }
    }
    CodeBegin += Delta;

    for (std::size_t Idx = PrevNumAllocations; Idx != Relocations.size(); ++Idx) {
      Relocations[Idx].Header.Offset += CodeBuffers.LatestOffset;
    }

    // Copy over CodeBuffer contents
    memcpy(GetCursorAddress<uint8_t*>(), TempCodeBuffer, TempSize);
    SetCursorOffset(CodeBuffers.LatestOffset + TempSize);

    CodeBuffers.LatestOffset = GetCursorOffset();

    // Memory this compile reused is fully repopulated now -- safe for any thread paused above to
    // resume into. See the matching BeginBufferInvalidationSafepoint call and its comment, above.
    // Release CodeInvalidationMutex (acquired exclusive, above, before the pause was opened) only
    // after ending the pause, then restore this thread's own shared reference to it, matching the
    // shared reference it held on entry to this reuse-in-place branch.
    if (HeldReuseSafepoint) {
      CTX->EndBufferInvalidationSafepoint(ThreadState);
      auto& InvalidationMutex = CTX->GetCodeInvalidationMutex();
      InvalidationMutex.unlock();
      InvalidationMutex.lock_shared();
    }
  }

  TempAllocator.DelayedDisownBuffer();

  // CodeBegin is a write (RW) address (see the comment on EntryPoint.second above -- unlike that
  // one, this pointer was never translated since it's normally "only used for symbol/debug
  // bookkeeping"). The actual memcpy of freshly-compiled bytes above happened through this RW
  // alias, so on iOS's dual-mapped scheme (RW and RX are different VAs backed by the same
  // physical pages) two separate cache-maintenance concerns apply, and both addresses are needed:
  //  1. The CPU's data cache may have dirtied a line tagged by the RW VA (the one actually
  //     written through) that a clean/invalidate issued only at the RX VA won't necessarily
  //     reach -- a classic VIPT synonym hazard for two virtual aliases of one physical page.
  //     Clearing at CodeBegin (RW) first flushes that dirtied line to the point of unification.
  //  2. The CPU's instruction cache may hold stale entries under the RX VA's own tag (e.g. from
  //     a previous occupant of this buffer slot), which only a clear issued at the RX address
  //     itself invalidates -- and RX is the alias actually fetched from when this code is later
  //     branched to, so it must be current.
  // Skipping either one reintroduces the same "branch into fresh code" instruction-fetch fault:
  // RX-only leaves the RW-side dirty line unflushed; RW-only leaves the RX-side icache stale.
  ClearICache(CodeBegin, CodeOnlySize);
  ClearICache(FEXCore::Allocator::GetExecutableAddress(CodeBegin), CodeOnlySize);

#ifdef VIXL_DISASSEMBLER
  if (Disassemble() & FEXCore::Config::Disassemble::STATS) {
    auto HeaderOp = IR->GetHeader();
    LOGMAN_THROW_A_FMT(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    LogMan::Msg::IFmt("RIP: 0x{:x}", Entry);
    LogMan::Msg::IFmt("Guest Code instructions: {}", HeaderOp->NumHostInstructions);
    LogMan::Msg::IFmt("Host Code instructions: {}", CodeOnlySize >> 2);
    LogMan::Msg::IFmt("Blow-up Amt: {}x", double(CodeOnlySize >> 2) / double(HeaderOp->NumHostInstructions));
  }

  if (Disassemble() & FEXCore::Config::Disassemble::BLOCKS) {
    const auto DisasmBegin = reinterpret_cast<const vixl::aarch64::Instruction*>(CodeBegin);
    const auto DisasmEnd = reinterpret_cast<const vixl::aarch64::Instruction*>(CodeBegin + CodeOnlySize);
    LogMan::Msg::IFmt("Disassemble Begin");
    for (auto PCToDecode = DisasmBegin; PCToDecode < DisasmEnd; PCToDecode += 4) {
      DisasmDecoder->Decode(PCToDecode);
      auto Output = Disasm->GetOutput();
      LogMan::Msg::IFmt("{}", Output);
    }
    LogMan::Msg::IFmt("Disassemble End");
  }
#endif

  DebugData->HostCodeSize = CodeData.Size;
  DebugData->Relocations = &Relocations;

  this->IR = nullptr;

  return std::move(CodeData);
}

void Arm64JITCore::ResetStack() {
  if (SpillSlots == 0) {
    return;
  }

  const auto TotalSpillSlotsSize = SpillSlots * MaxSpillSlotSize;

  if (ARMEmitter::IsImmAddSub(TotalSpillSlotsSize)) {
    add(ARMEmitter::Size::i64Bit, ARMEmitter::Reg::rsp, ARMEmitter::Reg::rsp, TotalSpillSlotsSize);
  } else {
    // Too big to fit in a 12bit immediate
    LoadConstant(ARMEmitter::Size::i64Bit, TMP1, TotalSpillSlotsSize);
    add(ARMEmitter::Size::i64Bit, ARMEmitter::XReg::rsp, ARMEmitter::XReg::rsp, TMP1, ARMEmitter::ExtendedType::LSL_64, 0);
  }
}

fextl::unique_ptr<CPUBackend> CreateArm64JITCore(FEXCore::Context::ContextImpl* ctx, FEXCore::Core::InternalThreadState* Thread) {
  return fextl::make_unique<Arm64JITCore>(ctx, Thread);
}

} // namespace FEXCore::CPU
