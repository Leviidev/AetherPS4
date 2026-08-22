// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/crash_reporter.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "common/singleton.h"
#include "core/fex/fex_guest_engine.h"
#include "core/libraries/kernel/memory.h"
#include "core/linker.h"
#include "core/memory.h"
#endif
#include "emulator.h"

#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#ifdef __APPLE__
#include <mach/arm/thread_status.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 32> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(_WIN32)

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    bool handled = false;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Breakpoints almost certainly come from our asserts/unreachables, no need to log it again.
    if (code != EXCEPTION_BREAKPOINT) {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGBUS:
    case SIGSEGV: {
        // Which of the two actually fired matters here: SIGBUS specifically (as opposed to
        // SIGSEGV) is the classic signature of accessing a mmap(MAP_SHARED)'d file-backed
        // region past the backing file's actual current size -- the mmap() call itself
        // succeeds either way, so this is invisible until something actually touches the
        // out-of-range page. Both cases funnel through this same switch arm, so without this
        // it's impossible to tell them apart from the rest of this handler's logging alone.
        LOG_CRITICAL(Debug, "SignalHandler: received {} ({})", sig,
                     sig == SIGBUS ? "SIGBUS" : (sig == SIGSEGV ? "SIGSEGV" : "?"));
        const bool is_write = Common::IsWriteError(raw_context);
        // Diagnostic: confirming whether DispatchAccessViolation's fan-out (GPU buffer
        // tracking / SRT walker JIT / x86 code patches -- see its own registrations)
        // claims this fault BEFORE HandleGuestSignal (the ARM64 unaligned-access
        // recovery) ever gets a chance to run. If it does, HandleGuestSignal is never
        // reached at all regardless of whether its own logic is correct.
        const bool dav_handled = signals->DispatchAccessViolation(raw_context, info->si_addr);
        LOG_CRITICAL(Debug, "SignalHandler: DispatchAccessViolation returned {}", dav_handled);
        if (dav_handled) {
            return;
        }
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        if (sig == SIGBUS) {
            // Checkpoint immediately before the call, matching the equally minimal
            // unconditional marker at the very top of HandleGuestSignal itself. If this
            // line appears in the log but the one inside HandleGuestSignal never does,
            // the break is in the call/trampoline transition itself, not in anything the
            // function's body does.
            constexpr char kCallMsg[] = "BACHATA_SIGHANDLER_PRECALL\n";
            write(STDERR_FILENO, kCallMsg, sizeof(kCallMsg) - 1);
            if (::AetherPS4::Fex::HandleGuestSignal(sig, info, raw_context)) {
                return;
            }
        }
        // A self-healing retry (re-requesting execute permission for an already-granted region
        // and resuming the same instruction) was tried here and pulled back out: every attempt
        // to re-request an ALREADY-granted region crashed identically regardless of how it was
        // issued (inline in this handler, or handed off to a dedicated background thread), while
        // the very same call always succeeds cleanly the first time for a fresh allocation. That
        // pattern -- fails 100% of the time, only ever on a second request for the same address --
        // points at re-preparing an already-granted region simply not being a supported
        // operation, not at a bug in how the call was issued.
        //
        // A distinct, narrower issue found via an actual on-device Minecraft crash: PC landing
        // exactly on the WRITABLE alias of a live JIT region instead of its executable one (some
        // JIT emission site still bakes in the wrong side of the dual mapping for one branch
        // target). See TryRecoverJitAliasFault's own comment for the full trace; unlike the
        // retry above, this doesn't ask StikDebug for anything new, it just corrects which
        // already-granted alias execution was about to use.
        if (::AetherPS4::Fex::TryRecoverJitAliasFault(sig, info, raw_context)) {
            return;
        }
#endif
        // If the guest has installed a custom signal handler, and the access violation didn't
        // come from HLE memory tracking, pass the signal on
        if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
            Libraries::Kernel::SigactionHandler(sig, info,
                                                reinterpret_cast<ucontext_t*>(raw_context));
            return;
        }
        Common::ReportCrash(raw_context, sig, info);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        uint64_t guest_rip = 0;
        uint64_t guest_rax = 0;
        if (::AetherPS4::Fex::BachataQueryGuestRipSyscall(&guest_rip, &guest_rax)) {
            LOG_CRITICAL(Debug, "FEX guest state at fault: rip={:#x} rax={:#x}", guest_rip,
                         guest_rax);
            // Which loaded module (main executable or .prx/.sprx) guest_rip falls inside, if
            // any -- narrows "which game/library code was actually running" beyond a bare
            // address, without needing a full guest-side symbolicator.
            if (auto* linker = Common::Singleton<Core::Linker>::Instance()) {
                if (auto* module = linker->FindByAddress(guest_rip)) {
                    LOG_CRITICAL(Debug,
                                 "FEX guest rip is inside module '{}' (base={:#x}, offset={:#x})",
                                 module->name, module->GetBaseAddress(),
                                 guest_rip - module->GetBaseAddress());
                } else {
                    LOG_CRITICAL(Debug, "FEX guest rip is not inside any loaded module");
                }
            }
        }
        // Classifies the faulting address (info->si_addr) against shadPS4's own guest virtual
        // memory manager, not just FEXCore's JIT tables above -- distinguishes "the VMM never
        // mapped this address at all" (a wild guest pointer, or a missing HLE mmap call the
        // game expected) from "the VMM's bookkeeping says this IS mapped, yet the host still
        // faulted here" (a host/guest memory-sync bug inside shadPS4 itself: the VMM's map and
        // the actual host page table have drifted apart). info->si_addr works directly as a
        // guest VAddr here since FEX identity-maps guest memory into the host process.
        if (auto* memory = Core::Memory::Instance()) {
            const auto guest_fault_addr = reinterpret_cast<VAddr>(info->si_addr);
            ::Libraries::Kernel::OrbisVirtualQueryInfo vma_info{};
            const auto query_result = memory->VirtualQuery(guest_fault_addr, 0, &vma_info);
            if (query_result == 0) {
                LOG_CRITICAL(Debug,
                             "FEX guest fault address classification: VMM says MAPPED "
                             "(vma {:#x}-{:#x}, prot={:#x}, name='{}', flexible={} direct={} "
                             "stack={} pooled={} committed={}) -- host/guest memory desync, not "
                             "a wild guest pointer",
                             vma_info.start, vma_info.end, vma_info.protection, vma_info.name,
                             static_cast<int>(vma_info.is_flexible),
                             static_cast<int>(vma_info.is_direct),
                             static_cast<int>(vma_info.is_stack),
                             static_cast<int>(vma_info.is_pooled),
                             static_cast<int>(vma_info.is_committed));
            } else {
                LOG_CRITICAL(Debug,
                             "FEX guest fault address classification: VMM says NOT mapped "
                             "(VirtualQuery returned {}) -- wild guest pointer, or a missing "
                             "mmap the game expected to have happened by now",
                             query_result);
            }
        }
        // Classifies the actual faulting HOST address (not the guest state above) against
        // FEXCore's iOS dual-mapped JIT allocation tables: is it the write side, the exec
        // side, or not a tracked JIT allocation at all (a wild pointer, or a stale address
        // into something already freed). Distinguishes "still an address-translation bug"
        // from "something else entirely" for the next crash in this class.
        char fault_desc[192] = {};
        if (::AetherPS4::Fex::BachataDescribeHostFaultAddress(info->si_addr, fault_desc,
                                                                sizeof(fault_desc))) {
            LOG_CRITICAL(Debug, "FEX host fault address classification: {}", fault_desc);
        }
        // Field-by-field comparison of the dispatcher's own known-good addresses against
        // what this thread's live per-thread pointer table actually holds for the same
        // fields -- a mismatch pinpoints a stale/corrupted/never-updated copy; matching
        // (wrong) values in both would instead point at Dispatcher.cpp itself.
        char dispatcher_state[768] = {};
        if (::AetherPS4::Fex::BachataDumpDispatcherState(dispatcher_state, sizeof(dispatcher_state))) {
            LOG_CRITICAL(Debug, "FEX dispatcher state: {}", dispatcher_state);
        }
#ifdef __APPLE__
        // LR (x30) at the fault is whoever branched into the bad address -- since ARM64
        // `br`/`blr` don't push a return address themselves, LR still holds whatever the
        // *caller's* last `bl`/`blr` set it to, which for FEXCore's hand-written dispatcher
        // assembly is normally the address right after that call site. Combined with the
        // fault address and the dispatcher_state dump above, this narrows down which of the
        // dispatcher's several "compile/link, then branch" sites (NoBlock, ExitFunctionLink
        // trampoline, CompileSingleStep -- see Dispatcher.cpp) was actually in flight.
        {
            auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
            const auto lr = static_cast<uintptr_t>(arm_thread_state64_get_lr(apple_context->uc_mcontext->__ss));
            const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(apple_context->uc_mcontext->__ss));
            LOG_CRITICAL(Debug, "FEX fault registers: pc={:#x} lr={:#x}", pc, lr);
        }
#endif
#endif
        UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                        fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                        fmt::ptr(info->si_addr));
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            Common::ReportCrash(raw_context, sig, info);
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            // Sleep thread until signal is received again
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#endif

SignalDispatch::SignalDispatch() {
    Common::InitCrashReporter();
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
}

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
