// SPDX-License-Identifier: MIT
#include "Interface/Context/Context.h"
#include "Interface/Core/OpcodeDispatcher.h"
#include "Interface/Core/Dispatcher/Dispatcher.h"
#include "Interface/Core/X86Tables/X86Tables.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CPUID.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <FEXCore/Core/Thunks.h>
#include "FEXCore/Debug/InternalThreadState.h"

#include <cstdio>

namespace FEXCore::Context {
fextl::unique_ptr<FEXCore::Context::Context> FEXCore::Context::Context::CreateNewContext(const FEXCore::HostFeatures& Features) {
  return fextl::make_unique<FEXCore::Context::ContextImpl>(Features);
}

void FEXCore::Context::ContextImpl::CompileRIP(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
  CompileBlock(Thread->CurrentFrame, GuestRIP);
}

void FEXCore::Context::ContextImpl::CompileRIPCount(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, uint64_t MaxInst) {
  CompileBlock(Thread->CurrentFrame, GuestRIP, MaxInst);
}

void FEXCore::Context::ContextImpl::SetSignalDelegator(FEXCore::SignalDelegator* _SignalDelegation) {
  SignalDelegation = _SignalDelegation;
}

void FEXCore::Context::ContextImpl::SetSyscallHandler(FEXCore::HLE::SyscallHandler* Handler) {
  SyscallHandler = Handler;
  SourcecodeResolver = Handler->GetSourcecodeResolver();
}

void FEXCore::Context::ContextImpl::SetThunkHandler(FEXCore::ThunkHandler* Handler) {
  ThunkHandler = Handler;
}

FEXCore::CPUID::FunctionResults FEXCore::Context::ContextImpl::RunCPUIDFunction(uint32_t Function, uint32_t Leaf) {
  return CPUID.RunFunction(Function, Leaf);
}

FEXCore::CPUID::XCRResults FEXCore::Context::ContextImpl::RunXCRFunction(uint32_t Function) {
  return CPUID.RunXCRFunction(Function);
}

FEXCore::CPUID::FunctionResults FEXCore::Context::ContextImpl::RunCPUIDFunctionName(uint32_t Function, uint32_t Leaf, uint32_t CPU) {
  return CPUID.RunFunctionName(Function, Leaf, CPU);
}

bool FEXCore::Context::ContextImpl::IsAddressInCodeBuffer(FEXCore::Core::InternalThreadState* Thread, uintptr_t Address) const {
  return Thread->CPUBackend->IsAddressInCodeBuffer(Address) || CodeCache.IsAddressInMappedCodeBuffer(Address);
}

bool FEXCore::Context::ContextImpl::DumpDispatcherStateForDiagnostics(FEXCore::Core::InternalThreadState* Thread, char* OutBuf,
                                                                       size_t OutBufSize) const {
  if (Thread == nullptr || Thread->CurrentFrame == nullptr || OutBuf == nullptr || OutBufSize == 0 || !Dispatcher) {
    return false;
  }
  const auto Known = Dispatcher->GetDiagnosticAddresses();
  const auto& Live = Thread->CurrentFrame->Pointers;

  // Field-by-field: dispatcher's known-good (freshly re-derived, always correct by
  // construction) value vs. whatever this thread's live Pointers struct actually holds for
  // the same conceptual address. A mismatch means InitThreadPointers() either never ran for
  // this field, ran before the dispatcher's own value was finalized, or something later wrote
  // over it -- as opposed to a dispatcher-level translation bug, which would show the *same*
  // (wrong) value in both columns.
  // ReuseGenerationCounterAddress and ExitFunctionLinkValidatedGeneration were never included
  // here before -- added specifically to test whether the iOS reuse-in-place generation-check
  // fallback (Dispatcher.cpp's ExitFunctionLinker, JIT.cpp's ExitFunctionLink) is actually wired
  // up for whichever thread crashes next. If ReuseGenerationCounterAddress reads 0 here, that
  // check is silently skipped entirely for this thread (cbz falls straight through to trusting
  // TMP1 unconditionally) -- a completely different bug from the race the mechanism itself is
  // designed to close, but one that would produce the exact same symptom (stale HostCode branch
  // target, "not tracked" JIT allocation fault).
  const uint64_t LiveGenValue = Live.ReuseGenerationCounterAddress != 0
    ? *reinterpret_cast<const volatile uint64_t*>(Live.ReuseGenerationCounterAddress)
    : 0;
  const int Written = std::snprintf(
    OutBuf, OutBufSize,
    "dispatcher known-good: DispatchPtr=%#llx Start=%#llx End=%#llx ExitFunctionLinker=%#llx "
    "DispatcherLoopTop=%#llx | "
    "thread live Pointers: DispatcherLoopTop=%#llx%s ExitFunctionLinker=%#llx%s "
    "DispatcherLoopTopFillSRA=%#llx DispatcherLoopTopEnterEC=%#llx DispatcherLoopTopEnterECFillSRA=%#llx "
    "SignalReturnHandler=%#llx SignalReturnHandlerRT=%#llx ThreadStopHandlerSpillSRA=%#llx "
    "ThreadPauseHandlerSpillSRA=%#llx | ReuseGenerationCounterAddress=%#llx%s live_gen=%llu "
    "ExitFunctionLinkValidatedGeneration=%llu",
    static_cast<unsigned long long>(Known.DispatchPtr), static_cast<unsigned long long>(Known.Start),
    static_cast<unsigned long long>(Known.End), static_cast<unsigned long long>(Known.ExitFunctionLinkerAddress),
    static_cast<unsigned long long>(Known.DispatcherLoopTopAddress),
    static_cast<unsigned long long>(Live.DispatcherLoopTop),
    (Live.DispatcherLoopTop == Known.DispatcherLoopTopAddress ? "(matches)" : "(MISMATCH)"),
    static_cast<unsigned long long>(Live.ExitFunctionLinker),
    (Live.ExitFunctionLinker == Known.ExitFunctionLinkerAddress ? "(matches)" : "(MISMATCH)"),
    static_cast<unsigned long long>(Live.DispatcherLoopTopFillSRA), static_cast<unsigned long long>(Live.DispatcherLoopTopEnterEC),
    static_cast<unsigned long long>(Live.DispatcherLoopTopEnterECFillSRA), static_cast<unsigned long long>(Live.SignalReturnHandler),
    static_cast<unsigned long long>(Live.SignalReturnHandlerRT), static_cast<unsigned long long>(Live.ThreadStopHandlerSpillSRA),
    static_cast<unsigned long long>(Live.ThreadPauseHandlerSpillSRA),
    static_cast<unsigned long long>(Live.ReuseGenerationCounterAddress),
    (Live.ReuseGenerationCounterAddress == 0 ? " NOT-WIRED-CHECK-SKIPPED" : ""),
    static_cast<unsigned long long>(LiveGenValue),
    static_cast<unsigned long long>(Thread->CurrentFrame->ExitFunctionLinkValidatedGeneration));
  return Written > 0;
}
} // namespace FEXCore::Context
