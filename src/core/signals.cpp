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
#include "core/ios/ios_jit_allocator.h"
#include "core/libraries/kernel/memory.h"
#include "core/linker.h"
#include "core/memory.h"
#endif
#include "emulator.h"

#include <cstdio>
#include <string_view>
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
            // Raw x86-64 opcode bytes at the faulting instruction itself: the register dump
            // below says which GPRs are live, but not which of them the actual faulting
            // instruction used as its base/index for the write -- these bytes are enough to
            // hand-decode the ModRM/SIB encoding and settle that without a disassembler.
            // guest_rip is directly host-readable, same as any other guest VAddr (FEX
            // identity-maps guest memory into this process); validated against the VMM first
            // since a wild/corrupted rip could point anywhere.
            if (auto* memory = Core::Memory::Instance()) {
                ::Libraries::Kernel::OrbisVirtualQueryInfo rip_vma{};
                if (memory->VirtualQuery(guest_rip, 0, &rip_vma) == 0) {
                    const auto* bytes =
                        reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(guest_rip));
                    char hex[64] = {};
                    char* w = hex;
                    for (int i = 0; i < 16; ++i) {
                        w += std::snprintf(w, hex + sizeof(hex) - w, "%02x ", bytes[i]);
                    }
                    LOG_CRITICAL(Debug, "FEX guest instruction bytes at rip: {}", hex);
                }
            }
        }
        // rip/rax alone weren't enough to tell what a NULL-pointer guest write actually came
        // from (which pointer was null, what called into the code that dereferenced it) --
        // the rest of the GPRs are whatever arguments/locals were live at the fault, and RSP
        // (returned separately below) lets the guest's own return-address chain be read out.
        char guest_regs[384] = {};
        uint64_t guest_rsp = 0;
        if (::AetherPS4::Fex::BachataDumpGuestRegisters(guest_regs, sizeof(guest_regs), &guest_rsp)) {
            LOG_CRITICAL(Debug, "FEX guest registers at fault: {}", guest_regs);
        }
        // Poor-man's guest backtrace: walk raw QWORDs upward from RSP and log whichever ones
        // land inside a loaded module's code range, on the (unverified but common for x86-64
        // -fno-omit-frame-pointer-less code) assumption that leftover return addresses are
        // still findable a few words up the stack even without a true frame-pointer walk.
        // Every word is validated against the VMM before being dereferenced, since RSP could
        // itself be garbage if the corruption reached the stack pointer.
        if (guest_rsp != 0) {
            if (auto* memory = Core::Memory::Instance()) {
                if (auto* linker = Common::Singleton<Core::Linker>::Instance()) {
                    constexpr int kStackWordsToScan = 64;
                    for (int i = 0; i < kStackWordsToScan; ++i) {
                        const auto word_addr = guest_rsp + static_cast<uint64_t>(i) * sizeof(uint64_t);
                        ::Libraries::Kernel::OrbisVirtualQueryInfo stack_vma{};
                        if (memory->VirtualQuery(word_addr, 0, &stack_vma) != 0) {
                            continue;
                        }
                        const auto word_value =
                            *reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(word_addr));
                        if (auto* module = linker->FindByAddress(word_value)) {
                            LOG_CRITICAL(Debug,
                                         "FEX guest stack[rsp+{:#x}]={:#x} -- inside module '{}' "
                                         "(offset={:#x}), likely a return address",
                                         i * sizeof(uint64_t), word_value, module->name,
                                         word_value - module->GetBaseAddress());
                        }
                    }
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
        // The earlier guest-instruction-bytes dump (at guest_rip) turned out to be a stale
        // JIT/HLE checkpoint, not the live fault site -- this dumps the actual ARM64 words at
        // code_address (the real host PC the signal landed on) instead. Fixed 4-byte-wide
        // instructions, unlike x86's variable length, so this is exact and hand-decodable.
        char host_code[256] = {};
        if (::AetherPS4::Fex::BachataDumpHostCodeWords(code_address, host_code, sizeof(host_code))) {
            LOG_CRITICAL(Debug, "FEX host ARM64 words around fault pc={:#x}: {}",
                         reinterpret_cast<uintptr_t>(code_address), host_code);
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
            // The "FEX guest state at fault" block earlier in this handler (guest rip, guest
            // instruction bytes, guest register dump) is a *stale* JIT/HLE checkpoint, not the
            // live fault site -- confirmed wrong on a real crash: it decoded as a completely
            // unrelated x86 instruction ("mov rsi, [rdi+0x20]") that couldn't be reconciled
            // with this fault's own info->si_addr at all. code_address/pc above is the real
            // ARM64 PC FEX's JIT was actually executing; x0-x28 (unlike pc/lr, not PAC-opaque,
            // so plain ts.__x[] access is correct -- same as flatten_extended_userdata_pass.cpp's
            // SrtWalkerSignalHandler) are the real host registers that instruction was operating
            // on, needed to compute what it actually faulted on instead of guessing from the
            // stale guest snapshot next time this happens.
            const auto& ts = apple_context->uc_mcontext->__ss;
            char host_regs[512] = {};
            int host_regs_len = 0;
            for (int i = 0; i < 29 && host_regs_len < static_cast<int>(sizeof(host_regs)) - 16;
                 i++) {
                host_regs_len += std::snprintf(host_regs + host_regs_len,
                                               sizeof(host_regs) - host_regs_len, "x%d=%#llx ", i,
                                               static_cast<unsigned long long>(ts.__x[i]));
            }
            LOG_CRITICAL(Debug, "FEX host ARM64 registers at fault: {}",
                        std::string_view(host_regs, host_regs_len));
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
#if defined(__APPLE__) && TARGET_OS_IPHONE
    case SIGTRAP: {
        // BreakGetJITMapping (ios_jit_allocator.cpp) issues a BRK #0xf00d instruction that
        // StikDebug's attached "Universal JIT Script" is supposed to intercept and service
        // before this handler ever sees it -- if it's working, this case never fires for that
        // call at all. It fires when StikDebug isn't there to catch the trap (confirmed
        // on-device: killed by iOS's own background wake-rate limiter mid-session -- see
        // ios_jit_allocator.cpp's g_expecting_jit_mapping_trap comment), which otherwise kills
        // the whole process outright with no return from get_jit_mapping() to recover through.
        // IsExpectingJitMappingTrap() is only true for the exact duration of that one call, so
        // this only ever recovers a BRK actually issued by it -- any other SIGTRAP (a real
        // debugger breakpoint) falls through to the same fatal path SIGILL uses above.
        if (Core::IosJitAllocator::IsExpectingJitMappingTrap()) {
            LOG_CRITICAL(Debug,
                        "BACHATA_JIT_TRAP_UNSERVICED: StikDebug did not service the "
                        "BreakGetJITMapping BRK (likely killed by iOS's background wake-rate "
                        "limiter) -- simulating a nullptr return instead of crashing");
            auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
            auto& ts = apple_context->uc_mcontext->__ss;
            // x0 is the ARM64 return-value register; simulating get_jit_mapping() == nullptr
            // here is exactly the failure path DualMappedRegion::Allocate() already handles
            // (logs, returns an invalid region -- the same as "BreakpointJIT unavailable").
            ts.__x[0] = 0;
            // BRK does not auto-advance PC the way e.g. x86's INT3 does -- PC still points at
            // the trapping instruction itself. Every ARM64 instruction is fixed 4 bytes wide,
            // so skipping exactly this one is unambiguous regardless of which library's code
            // (BreakpointJIT.framework's, not ours) the BRK actually lives in.
            const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(ts));
            arm_thread_state64_set_pc_fptr(ts, reinterpret_cast<void*>(pc + 4));
            return;
        }
        Common::ReportCrash(raw_context, sig, info);
        UNREACHABLE_MSG("Unhandled SIGTRAP at code address {} (not a JIT-mapping request)",
                        fmt::ptr(code_address));
    }
#endif
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
#if defined(__APPLE__) && TARGET_OS_IPHONE
    ASSERT_MSG(sigaction(SIGTRAP, &action, nullptr) == 0,
               "Failed to register JIT-mapping trap signal handler.");
#endif
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
