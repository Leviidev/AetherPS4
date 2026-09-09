// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/crash_reporter.h"
#include "common/decoder.h"
#include "common/logging/log.h"
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
#include "video_core/page_manager.h"
#endif
#include "emulator.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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
#include <mach-o/dyld.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 32> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(__APPLE__) && TARGET_OS_IPHONE
// Every raw-memory dump in this file runs *inside* a signal handler already reporting some
// other fault -- reading bytes/words around a fault address, walking a guest or native stack
// via [fp]/[fp+8], etc. A direct pointer dereference has no way to know a neighboring address
// is actually mapped, and a fault near the edge of a small/tightly-sized allocation (or a wild
// pointer used as a stack frame pointer) walks straight off it. Confirmed on-device: exactly
// this crashed with a SECOND SIGSEGV inside BachataDumpHostCodeWords itself (fex_guest_engine.cpp)
// while already handling a first one, visible in a symbolicated native call stack as that
// function appearing twice. vm_read_overwrite reports KERN_INVALID_ADDRESS instead of faulting
// when the source isn't mapped, so an unreadable byte/word becomes a visible gap in the
// diagnostic output rather than silently taking down the handler reporting the original crash --
// every direct dereference below one of these dumps read from was rewritten to use it.
// Deliberately the older vm_ (not mach_vm_) API: <mach/mach_vm.h> is unsupported on iOS
// ("#error mach_vm.h unsupported"), and mach_vm_read_overwrite isn't declared without it --
// vm_read_overwrite (declared in <mach/vm_map.h>, already reachable via <mach/mach.h>) does the
// same job with vm_address_t/vm_size_t, which are 64-bit on arm64 (LP64) so no truncation risk.
template <typename T>
bool BachataSafeRead(uintptr_t address, T* out) noexcept {
    vm_size_t bytes_read = 0;
    return vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(address), sizeof(T),
                             reinterpret_cast<vm_address_t>(out), &bytes_read) == KERN_SUCCESS &&
          bytes_read == sizeof(T);
}
#endif

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
        // One specific, fully-diagnosed Rocket League crash (see the function's own comment
        // for the three rounds of VMM-level diagnostics that ruled out everything else):
        // recovers by treating two known guest reads as if they'd loaded 0, exactly matching
        // what this same guest function already does when the equivalent pointer is
        // legitimately null. Deliberately placed last among the recovery attempts, and
        // deliberately narrow (checks the exact guest RIP before touching anything) so it
        // can't mask a genuinely different crash.
        if (::AetherPS4::Fex::TryRecoverKnownBadPropertyLink(sig, info, raw_context)) {
            return;
        }
        // FEXCore's call-return prediction cache genuinely running out of its own reserved
        // space (see the function's own comment for why no fixed size is ever truly enough,
        // and why resetting it is provably safe rather than a hack) -- checked last among the
        // recovery attempts since it does its own precise guard-page address check first and
        // can't mask an unrelated crash.
        if (::AetherPS4::Fex::TryRecoverCallRetStackOverflow(sig, info, raw_context)) {
            return;
        }
        // A guest direct-memory address computed against the SDK-standard fixed layout, but
        // never actually backed at that exact address on this platform -- see the function's
        // own comment for the full chain (MapMemory's rebase makes the mapping syscall succeed
        // elsewhere; the game separately dereferences the original address anyway) and why
        // three different attempts to place real memory at that exact address all failed.
        // Gated on the fault address itself, not a guess, so this can't mask an unrelated crash.
        if (::AetherPS4::Fex::TryRecoverDirectMemoryAddressMismatch(sig, info, raw_context)) {
            return;
        }
        // A specific, confirmed-deterministic GTA V (CUSA00419) crash inside eboot.bin+0x1c08f70:
        // a 5-level table walk (almost certainly RAGE's own address -> allocation-metadata
        // lookup) whose entry is missing for a real, rebased direct-memory pointer, and which
        // never null-checks before dereferencing. Recovers the whole lookup at once by
        // simulating an early "return 0" via FEXCore's own DispatcherLoopTopFillSRA redirect,
        // not by patching individual instructions. Gated on the exact guest rip range, so this
        // can't mask an unrelated crash. See the function's own comment for the full chain.
        if (::AetherPS4::Fex::TryRecoverNullResourceTableLookup(sig, info, raw_context)) {
            return;
        }
        // TryRecoverCorruptedGuestRsp (guest RIP 0x7001342320's rsp-corrupted-to-SceGnmDriver-
        // address crash) was tried and pulled back out: repairing rsp from rbp let the faulting
        // instruction itself succeed, but whatever actually corrupts rsp does so again almost
        // immediately on every subsequent call into this same function -- confirmed on-device
        // as an infinite recover/crash/recover loop (1M+ log lines, zero forward progress,
        // BACHATA_RSP_RECOVER and BACHATA_CALLRET_RESET alternating forever), which is worse
        // for the user than the clean crash this replaced: a crash is visible and recoverable
        // by relaunching, a silent infinite loop just hangs with the render thread still
        // re-presenting stale frames, looking alive. Falls through to the fatal path until the
        // actual corruption source (not yet found) is fixed instead of papered over.
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
        // Captured as early as possible on the genuinely-fatal path, before any of the other
        // (much slower, VirtualQuery-heavy) diagnostic logging below: confirmed on-device with
        // Rocket League that reading these code bytes from their old position, at the very end
        // of this handler, gave a disassembly that flatly contradicted the register dump taken
        // moments earlier (registers built a valid guest address into x18 immediately before
        // the store, yet the fault's own register snapshot showed x18=0 at that exact PC) --
        // the most likely explanation is another thread's JIT compilation overwrote this same
        // host code address in between, since nothing else here would explain instructions
        // that plainly do the opposite of what fired. Moving this read to right after
        // ReportCrash (this handler's only other work before it) minimizes that window.
        // Sized for BachataDumpHostCodeWords' current 24-words-before/24-after window: 49 words
        // at up to 9 chars each ("xxxxxxxx ") plus 2 extra chars for the bracketed word, rounded
        // up generously.
        char host_code[640] = {};
        if (::AetherPS4::Fex::BachataDumpHostCodeWords(code_address, host_code, sizeof(host_code))) {
            LOG_CRITICAL(Debug, "FEX host ARM64 words around fault pc={:#x}: {}",
                        reinterpret_cast<uintptr_t>(code_address), host_code);
        }
        // Byte-for-byte comparison against whatever was recorded at compile time for the one
        // known-deterministic block currently under investigation (see Core.cpp's
        // ContextImpl::CompileBlock and its BachataRecordKnownBlockSnapshot call) -- settles
        // definitively whether the disassembly above is still identical to what was originally
        // compiled, or whether this exact memory was rewritten sometime after.
        char snapshot_compare[256] = {};
        if (::AetherPS4::Fex::BachataCompareKnownBlockSnapshot(snapshot_compare, sizeof(snapshot_compare))) {
            LOG_CRITICAL(Debug, "FEX known-block snapshot comparison: {}", snapshot_compare);
        }
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
                if (memory->TryVirtualQuery(guest_rip, 0, &rip_vma)) {
                    char hex[64] = {};
                    char* w = hex;
                    for (int i = 0; i < 16; ++i) {
                        uint8_t byte = 0;
                        if (BachataSafeRead(static_cast<uintptr_t>(guest_rip) + i, &byte)) {
                            w += std::snprintf(w, hex + sizeof(hex) - w, "%02x ", byte);
                        } else {
                            w += std::snprintf(w, hex + sizeof(hex) - w, "?? ");
                        }
                    }
                    LOG_CRITICAL(Debug, "FEX guest instruction bytes at rip: {}", hex);
                }
            }
        }
        // guest_rip above is only the last JIT/HLE checkpoint (Frame->State.rip), which FEX
        // doesn't update on every instruction while running straight-line JIT code -- for a
        // fault deep inside a large, branch-free block (confirmed to be this block's actual
        // shape: ~25KB of compiled code with no internal branches at all) that checkpoint is
        // just the block's entry point, not the faulting instruction, which could be thousands
        // of bytes further in. This calls into FEXCore's own per-block RIP-entries side table
        // (via the newly-fixed ContextImpl::RestoreRIPFromHostPC -- see its Core.cpp comment)
        // using code_address, the actual host fault PC, to get the exact guest instruction.
        uint64_t accurate_guest_rip = 0;
        if (::AetherPS4::Fex::BachataReconstructAccurateGuestRIP(code_address, &accurate_guest_rip)) {
            LOG_CRITICAL(Debug, "FEX accurate guest rip at fault (reconstructed from host pc): {:#x}",
                         accurate_guest_rip);
            // guest_rip above (from BachataQueryGuestRipSyscall) is only the last JIT/HLE
            // checkpoint and can be stale by the time of an actual fault (see its own comment
            // above) -- accurate_guest_rip is the address that actually faulted, so the "which
            // module" check belongs here too, not only on the potentially-stale value. A GTA V
            // crash showed these two disagree entirely (guest_rip landed near an HLE veneer,
            // which Linker legitimately doesn't track, while accurate_guest_rip was a real
            // address inside eboot.bin) -- checking only guest_rip made a real in-module crash
            // misleadingly print "not inside any loaded module".
            if (auto* linker = Common::Singleton<Core::Linker>::Instance()) {
                if (auto* module = linker->FindByAddress(accurate_guest_rip)) {
                    LOG_CRITICAL(Debug,
                                 "FEX accurate guest rip is inside module '{}' (base={:#x}, offset={:#x})",
                                 module->name, module->GetBaseAddress(),
                                 accurate_guest_rip - module->GetBaseAddress());
                } else {
                    LOG_CRITICAL(Debug, "FEX accurate guest rip is not inside any loaded module");
                }
            }
            if (auto* memory = Core::Memory::Instance()) {
                ::Libraries::Kernel::OrbisVirtualQueryInfo rip_vma{};
                if (memory->TryVirtualQuery(accurate_guest_rip, 0, &rip_vma)) {
                    // Widened from a 16-byte peek to a 1KB window (512 before/after) to see the
                    // whole containing function, not just the faulting instruction itself --
                    // this same accurate RIP has now faulted identically across multiple runs
                    // with a different (but always +0x17c off the fault address) RDX value each
                    // time, meaning the crash isn't in this instruction's own decode/codegen at
                    // all: something *earlier* in this function computed a bad pointer into
                    // RDX, an SRA-persistent register (not a transient scratch like the x18 bug
                    // was), and this is just the first place that dereferences it. Seeing the
                    // full function is what's needed to find that earlier computation.
                    constexpr uint64_t kWindowBefore = 512;
                    constexpr uint64_t kWindowAfter = 512;
                    const auto window_start = static_cast<uintptr_t>(accurate_guest_rip - kWindowBefore);
                    static char hex[2 * (kWindowBefore + kWindowAfter) + 1] = {};
                    char* w = hex;
                    for (uint64_t i = 0; i < kWindowBefore + kWindowAfter; ++i) {
                        uint8_t byte = 0;
                        if (BachataSafeRead(window_start + i, &byte)) {
                            w += std::snprintf(w, hex + sizeof(hex) - w, "%02x", byte);
                        } else {
                            w += std::snprintf(w, hex + sizeof(hex) - w, "??");
                        }
                    }
                    LOG_CRITICAL(Debug,
                                 "FEX guest function window at accurate rip: rip={:#x} "
                                 "window_start={:#x} before={:#x} bytes={}",
                                 accurate_guest_rip, accurate_guest_rip - kWindowBefore,
                                 kWindowBefore, hex);
                }
            }
        }
        // rip/rax alone weren't enough to tell what a NULL-pointer guest write actually came
        // from (which pointer was null, what called into the code that dereferenced it) --
        // the rest of the GPRs are whatever arguments/locals were live at the fault, and RSP
        // (returned separately below) lets the guest's own return-address chain be read out.
        char guest_regs[384] = {};
        uint64_t guest_rsp = 0;
        uint64_t guest_rbp = 0;
        if (::AetherPS4::Fex::BachataDumpGuestRegisters(guest_regs, sizeof(guest_regs), &guest_rsp,
                                                        &guest_rbp)) {
            LOG_CRITICAL(Debug, "FEX guest registers at fault: {}", guest_regs);
        }
        // Diagnostic only, gated to the one known-deterministic crash under investigation (guest
        // RIP 0x7000766630). The RSP-scan "poor-man's backtrace" below is explicitly unverified
        // -- confirmed unreliable here on-device: its first hit (rsp+0x8, into libc.prx) turned
        // out to be a stale leftover from an unrelated, already-returned call (disassembling that
        // call site showed a completely ordinary, unrelated argument setup, nothing pointing at
        // this destructor at all). This function's own disassembly confirms it maintains a real
        // frame (push rbp; mov rbp, rsp at entry), so a genuine [rbp]/[rbp+8] walk should recover
        // the *actual* caller chain instead of guessing from whatever's left on the stack.
        if (accurate_guest_rip == 0x7000766630ULL && guest_rbp != 0) {
            if (auto* memory = Core::Memory::Instance()) {
                if (auto* linker = Common::Singleton<Core::Linker>::Instance()) {
                    uint64_t frame_ptr = guest_rbp;
                    for (int depth = 0; depth < 8; ++depth) {
                        ::Libraries::Kernel::OrbisVirtualQueryInfo saved_rbp_vma{};
                        ::Libraries::Kernel::OrbisVirtualQueryInfo ret_addr_vma{};
                        if (!memory->TryVirtualQuery(frame_ptr, 0, &saved_rbp_vma) ||
                            !memory->TryVirtualQuery(frame_ptr + 8, 0, &ret_addr_vma)) {
                            LOG_CRITICAL(Debug, "FEX rbp-chain[{}]: frame_ptr={:#x} not mapped, stopping",
                                         depth, frame_ptr);
                            break;
                        }
                        uint64_t saved_rbp = 0;
                        uint64_t ret_addr = 0;
                        if (!BachataSafeRead(static_cast<uintptr_t>(frame_ptr), &saved_rbp) ||
                            !BachataSafeRead(static_cast<uintptr_t>(frame_ptr + 8), &ret_addr)) {
                            LOG_CRITICAL(Debug, "FEX rbp-chain[{}]: frame_ptr={:#x} unreadable, stopping",
                                        depth, frame_ptr);
                            break;
                        }
                        if (auto* module = linker->FindByAddress(ret_addr)) {
                            LOG_CRITICAL(Debug,
                                         "FEX rbp-chain[{}]: frame_ptr={:#x} saved_rbp={:#x} "
                                         "return_addr={:#x} -- inside module '{}' (offset={:#x})",
                                         depth, frame_ptr, saved_rbp, ret_addr, module->name,
                                         ret_addr - module->GetBaseAddress());
                            // Diagnostic only: depth 0 here is the *actual*, frame-pointer-
                            // verified immediate caller of the destructor under investigation
                            // (guest RIP 0x7000766630) -- unlike the RSP-scan's first hit, which
                            // turned out to be a stale, unrelated leftover. Dump its code window
                            // to see the real argument setup for the call that reaches this
                            // destructor with a corrupted `this` (RBX=1). Depth 0's own call site
                            // (libc.prx+0x26731's caller, confirmed via disassembly) never
                            // reassigns RDI before forwarding it onward -- RDI/RDX both carry
                            // depth 0's *own* incoming RDI straight through to the callee. That
                            // means whatever's wrong with `this` was already wrong before depth 0
                            // even ran; dump depth 1 (its own caller) too, to find where RDI
                            // actually gets set to something bad in the first place.
                            if (depth == 0 || depth == 1) {
                                constexpr uint64_t kCallerWindowBefore = 256;
                                constexpr uint64_t kCallerWindowAfter = 64;
                                const auto caller_window_start = static_cast<uintptr_t>(ret_addr - kCallerWindowBefore);
                                static char caller_hex[2 * (kCallerWindowBefore + kCallerWindowAfter) + 1] = {};
                                char* cw = caller_hex;
                                for (uint64_t j = 0; j < kCallerWindowBefore + kCallerWindowAfter; ++j) {
                                    uint8_t byte = 0;
                                    if (BachataSafeRead(caller_window_start + j, &byte)) {
                                        cw += std::snprintf(cw, caller_hex + sizeof(caller_hex) - cw, "%02x", byte);
                                    } else {
                                        cw += std::snprintf(cw, caller_hex + sizeof(caller_hex) - cw, "??");
                                    }
                                }
                                LOG_CRITICAL(Debug,
                                             "FEX rbp-verified caller window: return_addr={:#x} "
                                             "window_start={:#x} before={:#x} bytes={}",
                                             ret_addr, ret_addr - kCallerWindowBefore, kCallerWindowBefore,
                                             caller_hex);
                            }
                        } else {
                            LOG_CRITICAL(Debug,
                                         "FEX rbp-chain[{}]: frame_ptr={:#x} saved_rbp={:#x} "
                                         "return_addr={:#x} -- no module match",
                                         depth, frame_ptr, saved_rbp, ret_addr);
                        }
                        if (saved_rbp == 0 || saved_rbp <= frame_ptr) {
                            break;
                        }
                        frame_ptr = saved_rbp;
                    }
                }
            }
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
                        if (!memory->TryVirtualQuery(word_addr, 0, &stack_vma)) {
                            continue;
                        }
                        uint64_t word_value = 0;
                        if (!BachataSafeRead(static_cast<uintptr_t>(word_addr), &word_value)) {
                            continue;
                        }
                        if (auto* module = linker->FindByAddress(word_value)) {
                            LOG_CRITICAL(Debug,
                                         "FEX guest stack[rsp+{:#x}]={:#x} -- inside module '{}' "
                                         "(offset={:#x}), likely a return address",
                                         i * sizeof(uint64_t), word_value, module->name,
                                         word_value - module->GetBaseAddress());
                            // The rbp-chain walk above (frame-pointer-verified) already dumps
                            // the real immediate caller's code window for this same crash --
                            // this RSP-scan hit turned out to point at an unrelated, already-
                            // returned call (confirmed on-device: disassembling it showed a
                            // completely ordinary, unrelated argument setup), so it's no longer
                            // trusted as a caller-identification source. Left as a plain log
                            // line only, for whatever residual value the raw stack contents have.
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
            const bool query_result = memory->TryVirtualQuery(guest_fault_addr, 0, &vma_info);
            if (query_result) {
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
                // "VMM says mapped, host still faulted" is exactly the signature of a race
                // between this read and a PoolCommit/PoolDecommit of the same range on another
                // thread -- VirtualQuery's own answer above is correctly synchronized against
                // the VMM's bookkeeping (see memory.h's DumpRecentPoolOps comment), but that
                // says nothing about what raced the *actual* host memory access, which goes
                // through a raw pointer with no lock at all. This dumps the last 64
                // commit/decommit calls so a recent one overlapping this exact address is
                // visible directly instead of inferred.
                char pool_ops[8192] = {};
                memory->DumpRecentPoolOps(guest_fault_addr, pool_ops, sizeof(pool_ops));
                LOG_CRITICAL(Debug, "FEX recent pool commit/decommit history: {}", pool_ops);
                // A second, separate possible source of desync: video_core's PageManager calls
                // real mprotect() to write/read-protect GPU-tracked buffer pages, entirely
                // independent of the VMM's own commit/decommit bookkeeping above. On Apple
                // platforms that mprotect gets rounded out to the full 16KB host page (4 PS4
                // pages), so protecting one GPU-tracked page can collaterally restrict an
                // unrelated neighbor -- see page_manager.h's DumpRecentPageProtects comment.
                char page_protects[8192] = {};
                if (VideoCore::PageManager::DumpRecentPageProtects(
                        guest_fault_addr, page_protects, sizeof(page_protects))) {
                    LOG_CRITICAL(Debug, "FEX recent GPU page-protect history: {}", page_protects);
                }
                // Both diagnostics above came back empty across multiple crashes of this same
                // shape, ruling out both a commit/decommit race and a GPU page-protect
                // collateral-restriction race. This VMA's range, as VirtualQuery reports it, is
                // a *merged* view -- Direct-type VMAs coalesce adjacent MapMemory calls (see
                // memory.h's DumpRecentMapOps comment) -- so this checks whether the exact
                // faulting sub-range was ever actually covered by one of those calls, or whether
                // it's a genuine gap inside an otherwise-merged tracking entry.
                static char map_ops[16384] = {};
                memory->DumpRecentMapOps(guest_fault_addr, map_ops, sizeof(map_ops));
                LOG_CRITICAL(Debug, "FEX recent Direct MapMemory history: {}", map_ops);
            } else {
                LOG_CRITICAL(Debug,
                             "FEX guest fault address classification: VMM says NOT mapped "
                             "(or its lock wasn't immediately available) -- wild guest pointer, "
                             "a missing mmap the game expected to have happened by now, or this "
                             "same thread already held the VMM lock when it faulted");
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
            // Confirmed on-device that manually re-deriving this mapping per-crash (as this
            // whole comment block above already documents doing once, for RSP/x8 specifically)
            // is a real, recurring cost -- a GTA V crash whose "guest registers at fault" line
            // showed rcx=0x30 turned out, once actually cross-referenced against FEXCore's own
            // x64::SRA table (Arm64Emitter.cpp -- Apple/non-arm64ec branch), to have the live
            // RCX sitting in x7 reading 0x7350 instead: a completely different value, because
            // the stale-checkpoint problem this comment block describes for guest_rip/guest
            // registers applies to every SRA-mapped GPR, not just RSP. Doing that translation
            // here automatically, from the same already-read ts.__x[], removes the need to
            // redo it by hand next time. R15's slot is r29 (Apple's opaque fp field, not part
            // of __x[0..28]), read via arm_thread_state64_get_fp same as this function already
            // does for LR above.
            LOG_CRITICAL(Debug,
                        "FEX live x86 GPRs (via SRA, more reliable than the stale guest-state "
                        "dump above for a fault deep in a block): RAX={:#x} RCX={:#x} "
                        "RDX={:#x} RBX={:#x} RSP={:#x} RBP={:#x} RSI={:#x} "
                        "RDI={:#x} R8={:#x} R9={:#x} R10={:#x} R11={:#x} R12={:#x} "
                        "R13={:#x} R14={:#x} R15={:#x}",
                        static_cast<unsigned long long>(ts.__x[4]),
                        static_cast<unsigned long long>(ts.__x[7]),
                        static_cast<unsigned long long>(ts.__x[5]),
                        static_cast<unsigned long long>(ts.__x[6]),
                        static_cast<unsigned long long>(ts.__x[8]),
                        static_cast<unsigned long long>(ts.__x[9]),
                        static_cast<unsigned long long>(ts.__x[10]),
                        static_cast<unsigned long long>(ts.__x[11]),
                        static_cast<unsigned long long>(ts.__x[12]),
                        static_cast<unsigned long long>(ts.__x[13]),
                        static_cast<unsigned long long>(ts.__x[14]),
                        static_cast<unsigned long long>(ts.__x[15]),
                        static_cast<unsigned long long>(ts.__x[16]),
                        static_cast<unsigned long long>(ts.__x[17]),
                        static_cast<unsigned long long>(ts.__x[19]),
                        static_cast<unsigned long long>(arm_thread_state64_get_fp(ts)));
            // New crash class (guest RIP 0x7001342320): a SIGBUS write landing 8 bytes below
            // whatever this compiled block's host x8 register holds. x8 is guest RSP's fixed SRA
            // slot on this build -- FEXCore's x64::SRA table (Arm64Emitter.cpp) maps
            // X86State::REG_RSP (index 4 into CurrentFrame->State.gregs) to ARMEmitter::Reg::r8,
            // and the faulting instruction here decodes as a genuine x86 "push rbp" at a
            // 16-byte-aligned function entry, which by x86 semantics can only ever operate on
            // RSP -- so whatever's live in x8 at this exact PC is what FEX believes the guest
            // stack pointer is. Across two separate crash logs of this exact signature, that
            // value was byte-for-byte identical to an unrelated sceKernelMapNamedDirectMemory
            // allocation's returned address logged ~15,000 lines earlier in the same session,
            // suggesting guest RSP had somehow ended up holding a heap pointer instead of a
            // stack address. Rather than trust that as an eyeballed coincidence again, this
            // queries the VMM directly for what every SRA-mapped guest GPR currently points to
            // (not just RSP) -- settles definitively whether it's heap/direct memory, a real
            // guest stack at a wildly wrong offset, or something else, and whether the
            // corruption is isolated to RSP alone or spans the whole live register file (the
            // latter would point at a bad State reload/resync rather than something RSP-specific).
            if (accurate_guest_rip == 0x7001342320ULL) {
                if (auto* memory = Core::Memory::Instance()) {
                    struct SraSlot {
                        const char* guest_name;
                        int host_reg;
                    };
                    // Order/registers must match x64::SRA in Arm64Emitter.cpp: guest GPR index i
                    // (X86State::REG_*) lives in physical host register SRA[i].
                    static constexpr SraSlot kSraSlots[] = {
                        {"rax", 4},  {"rcx", 7},  {"rdx", 5},  {"rbx", 6},
                        {"rsp", 8},  {"rbp", 9},  {"rsi", 10}, {"rdi", 11},
                        {"r8", 12},  {"r9", 13},  {"r10", 14}, {"r11", 15},
                        {"r12", 16}, {"r13", 17}, {"r14", 19}, {"r15", 29},
                    };
                    for (const auto& slot : kSraSlots) {
                        const auto value = static_cast<VAddr>(ts.__x[slot.host_reg]);
                        ::Libraries::Kernel::OrbisVirtualQueryInfo vma{};
                        if (memory->TryVirtualQuery(value, 0, &vma)) {
                            LOG_CRITICAL(Debug,
                                         "BACHATA_SRA_PROBE: guest {}=x{}={:#x} -- mapped VMA "
                                         "{:#x}-{:#x} offset_into_vma={:#x} name='{}' "
                                         "stack={} direct={} flexible={} pooled={}",
                                         slot.guest_name, slot.host_reg, value, vma.start, vma.end,
                                         value - vma.start, vma.name,
                                         static_cast<int>(vma.is_stack),
                                         static_cast<int>(vma.is_direct),
                                         static_cast<int>(vma.is_flexible),
                                         static_cast<int>(vma.is_pooled));
                        } else {
                            LOG_CRITICAL(Debug,
                                         "BACHATA_SRA_PROBE: guest {}=x{}={:#x} -- not inside any "
                                         "tracked VMA",
                                         slot.guest_name, slot.host_reg, value);
                        }
                    }
                }
                // BACHATA_SRA_PROBE settled that this crash isn't stack exhaustion after all --
                // guest rbp (x9) sits only ~32KB into the (now 8MB) main stack, nowhere near
                // deep enough for genuine overflow, while guest rsp (x8) is corrupted to exactly
                // the unrelated "SceGnmDriver" allocation's base address. Since x9/rbp is
                // confirmed valid here (unlike x8), a [rbp]/[rbp+8] chain walk from it -- same
                // proven technique as the rbp-chain walk gated on 0x7000766630 above, just using
                // the *live* register here instead of that one's stale BachataDumpGuestRegisters
                // snapshot -- should recover the real caller chain into this corrupted-rsp call,
                // rather than guessing from a register that's already known to be wrong.
                if (accurate_guest_rip == 0x7001342320ULL) {
                    auto* memory = Core::Memory::Instance();
                    auto* linker = Common::Singleton<Core::Linker>::Instance();
                    if (memory != nullptr && linker != nullptr) {
                        uint64_t frame_ptr = ts.__x[9];
                        for (int depth = 0; depth < 8 && frame_ptr != 0; ++depth) {
                            ::Libraries::Kernel::OrbisVirtualQueryInfo saved_rbp_vma{};
                            ::Libraries::Kernel::OrbisVirtualQueryInfo ret_addr_vma{};
                            if (!memory->TryVirtualQuery(frame_ptr, 0, &saved_rbp_vma) ||
                                !memory->TryVirtualQuery(frame_ptr + 8, 0, &ret_addr_vma)) {
                                LOG_CRITICAL(Debug,
                                             "FEX rsp-corrupt rbp-chain[{}]: frame_ptr={:#x} not "
                                             "mapped, stopping",
                                             depth, frame_ptr);
                                break;
                            }
                            uint64_t saved_rbp = 0;
                            uint64_t ret_addr = 0;
                            if (!BachataSafeRead(static_cast<uintptr_t>(frame_ptr), &saved_rbp) ||
                                !BachataSafeRead(static_cast<uintptr_t>(frame_ptr + 8), &ret_addr)) {
                                LOG_CRITICAL(Debug,
                                             "FEX rsp-corrupt rbp-chain[{}]: frame_ptr={:#x} "
                                             "unreadable, stopping",
                                             depth, frame_ptr);
                                break;
                            }
                            if (auto* module = linker->FindByAddress(ret_addr)) {
                                LOG_CRITICAL(Debug,
                                             "FEX rsp-corrupt rbp-chain[{}]: frame_ptr={:#x} "
                                             "saved_rbp={:#x} return_addr={:#x} -- inside module "
                                             "'{}' (offset={:#x})",
                                             depth, frame_ptr, saved_rbp, ret_addr, module->name,
                                             ret_addr - module->GetBaseAddress());
                            } else {
                                LOG_CRITICAL(Debug,
                                             "FEX rsp-corrupt rbp-chain[{}]: frame_ptr={:#x} "
                                             "saved_rbp={:#x} return_addr={:#x} -- no module match",
                                             depth, frame_ptr, saved_rbp, ret_addr);
                            }
                            if (saved_rbp == 0 || saved_rbp <= frame_ptr) {
                                break;
                            }
                            frame_ptr = saved_rbp;
                        }
                    }
                }
            }
            // A newly-seen crash shape: LR at fault pointed into the dispatcher's own
            // ExitFunctionLinker assembly at the exact return site of its call into the native
            // C++ ExitFunctionLink function -- i.e. this fault may be happening inside *native*
            // code (ExitFunctionLink, or CompileBlock/CompileCode/IR emission that it calls on
            // its slow path), not inside guest-JIT code at all, which the existing "not tracked
            // JIT allocation" classification can't distinguish (native code is always
            // "untracked" by that check, whether or not that's actually the bug). Standard
            // AAPCS64 frames chain via [fp]=saved fp, [fp+8]=saved lr; walking it gives real
            // native return addresses to symbolicate offline against this exact build's binary
            // (matching dSYM/UUID from the shipped IPA), which settles definitively whether
            // this is native call-stack corruption/a bad function pointer versus the familiar
            // guest-JIT-branch-staleness class of bug.
            uintptr_t frame_fp = static_cast<uintptr_t>(arm_thread_state64_get_fp(ts));
            char native_bt[1024] = {};
            int native_bt_len = 0;
            for (int depth = 0; depth < 24 && frame_fp != 0 &&
                 native_bt_len < static_cast<int>(sizeof(native_bt)) - 32;
                 depth++) {
                // fp must itself look like a plausible stack address before dereferencing it --
                // a corrupted chain (which is exactly one of the hypotheses this is chasing)
                // could easily hand back garbage, and this whole dump is diagnostic-only, not
                // worth a second fault over.
                if (frame_fp < 0x1000 || (frame_fp & 0x7) != 0) {
                    break;
                }
                uintptr_t saved_fp = 0;
                uintptr_t saved_lr = 0;
                if (!BachataSafeRead(frame_fp, &saved_fp) || !BachataSafeRead(frame_fp + sizeof(uintptr_t), &saved_lr)) {
                    break;
                }
                native_bt_len += std::snprintf(native_bt + native_bt_len,
                                               sizeof(native_bt) - native_bt_len, "[%d]=%#llx ",
                                               depth, static_cast<unsigned long long>(saved_lr));
                if (saved_lr == 0 || saved_fp == frame_fp) {
                    break;
                }
                frame_fp = saved_fp;
            }
            LOG_CRITICAL(Debug, "FEX native ARM64 call stack (walk [fp]/[fp+8], symbolicate "
                                "offline against this build's binary): {}",
                        std::string_view(native_bt, native_bt_len));
        }
#endif
#endif
        // Diagnostic only: several GTA V crashes at this exact fault PC (0x1ce850a7c, a system
        // library's thread-local-storage-destructor-chain walker) read a "next" pointer field
        // whose *value* decoded as fragments of readable text ("d pass 0", "t pass 0", "...s
        // 0\0pass...") instead of a real pointer -- confirmed not a valid canonical address at
        // all (bit pattern exceeds the 48-bit address space), so dumping around the fault
        // address itself (info->si_addr, computed from that same bad value) would only ever
        // read unmapped memory. The stack, by contrast, is guaranteed valid here and may still
        // hold whatever address that corrupted value was originally loaded from (e.g. spilled
        // to a stack slot before this loop's own load), or other nearby pointers into the same
        // corrupted region -- this is genuinely a fishing expedition, but a free one, and it's
        // the only guaranteed-readable memory left to look at from here.
        {
            auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
            auto& ts = apple_context->uc_mcontext->__ss;
            constexpr int kBytesBefore = 64;
            constexpr int kBytesAfter = 192;
            const auto sp_addr = static_cast<uintptr_t>(arm_thread_state64_get_sp(ts));
            char dump[3 * (kBytesBefore + kBytesAfter) + 16] = {};
            int dump_len = 0;
            for (int i = -kBytesBefore; i < kBytesAfter && dump_len < static_cast<int>(sizeof(dump)) - 8; ++i) {
                uint8_t byte = 0;
                const auto addr = static_cast<uintptr_t>(static_cast<ptrdiff_t>(sp_addr) + i);
                if (BachataSafeRead(addr, &byte)) {
                    dump_len += std::snprintf(dump + dump_len, sizeof(dump) - dump_len, "%02x", byte);
                } else {
                    dump_len += std::snprintf(dump + dump_len, sizeof(dump) - dump_len, "??");
                }
            }
            LOG_CRITICAL(Debug,
                        "BACHATA_FAULT_STACK_MEMORY: {} bytes before, {} after sp={} (hex, ?? = "
                        "unreadable): {}",
                        kBytesBefore, kBytesAfter, fmt::ptr(reinterpret_cast<void*>(sp_addr)),
                        std::string_view(dump, dump_len));
        }
        // Targeted flush right at the actual point of no return, not on every LOG_CRITICAL
        // globally (see log.cpp's comment on flush_on) -- UNREACHABLE_MSG's own abort() skips
        // every normal exit hook that would otherwise flush this, and everything logged above
        // in this handler is worthless if it never reaches disk.
        Common::Log::Flush();
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
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
            // First Bloodborne crash investigated on this build: unlike every SIGBUS this file
            // already has rich diagnostics for (a bad memory *address*), SIGILL means the CPU
            // refused to execute whatever instruction is actually sitting at code_address --
            // DisassembleInstruction is a no-op stub on this ARM64 host (Zydis is only wired up
            // for ARCH_X86_64, see its #ifdef above), so "<unable to decode>" told us nothing.
            // Mirrors the SIGBUS handler's own diagnostic set below, since the same open
            // questions apply here: is code_address even inside a live JIT allocation, what
            // guest x86 code was this block translating, and -- the one most specific to SIGILL
            // -- do the actual bytes at the fault match what was compiled, or did something
            // overwrite this address with garbage after the fact (memory corruption) rather
            // than the JIT genuinely emitting an invalid instruction (a miscompilation).
            char ill_host_code[640] = {};
            if (::AetherPS4::Fex::BachataDumpHostCodeWords(code_address, ill_host_code, sizeof(ill_host_code))) {
                LOG_CRITICAL(Debug, "FEX SIGILL host ARM64 words around fault pc={:#x}: {}",
                            reinterpret_cast<uintptr_t>(code_address), ill_host_code);
            }
            // Bloodborne investigation: every occurrence of this crash lands at exactly
            // thunk_start+0x1c (confirmed across multiple sessions via BACHATA_BLOODBORNE_
            // THUNK_TRACE, which independently proved the crashing thunk's caller BL instruction
            // was correctly encoded (branching to thunk_start+0x00) at compile time. That rules
            // out the emission/binding path -- if the caller's instruction is now something
            // *other* than that original BL, whatever rewrote it is the actual bug (a
            // DirectBlockDelinker/ExitFunctionLink direct-patch computing the wrong target, or a
            // stray write from something else entirely); if it's unchanged, the corruption must
            // be happening some other way this doesn't yet explain (e.g. the CPU never actually
            // took this exact BL, meaning something else branched here directly).
            {
                const auto fault_addr = reinterpret_cast<uintptr_t>(code_address);
                const auto thunk_start = fault_addr - 0x1c;
                uint64_t bb_guest_rip = 0;
                if (BachataSafeRead(thunk_start + 0x18, &bb_guest_rip) && bb_guest_rip == 0x7002047b00ULL) {
                    uint64_t bb_host_code = 0;
                    int64_t bb_caller_offset = 0;
                    uint32_t bb_caller_word = 0;
                    const bool got_host_code = BachataSafeRead(thunk_start + 0x10, &bb_host_code);
                    const bool got_caller_offset = BachataSafeRead(thunk_start + 0x20, &bb_caller_offset);
                    const bool got_caller_word =
                        got_caller_offset &&
                        BachataSafeRead(static_cast<uintptr_t>(static_cast<int64_t>(thunk_start) + bb_caller_offset), &bb_caller_word);
                    LOG_CRITICAL(Debug,
                                "BACHATA_BLOODBORNE_CALLER_AT_CRASH: thunk_start={:#x} HostCode={:#x} "
                                "(read_ok={}) CallerOffset={} (read_ok={}) CallerAddress={:#x} "
                                "CallerWordNow={:#x} (read_ok={})",
                                thunk_start, bb_host_code, got_host_code, bb_caller_offset, got_caller_offset,
                                got_caller_offset ? static_cast<uintptr_t>(static_cast<int64_t>(thunk_start) + bb_caller_offset) : 0,
                                bb_caller_word, got_caller_word);
                }
            }
            char ill_snapshot_compare[256] = {};
            if (::AetherPS4::Fex::BachataCompareKnownBlockSnapshot(ill_snapshot_compare, sizeof(ill_snapshot_compare))) {
                LOG_CRITICAL(Debug, "FEX SIGILL known-block snapshot comparison: {}", ill_snapshot_compare);
            }
            char ill_fault_desc[192] = {};
            if (::AetherPS4::Fex::BachataDescribeHostFaultAddress(code_address, ill_fault_desc, sizeof(ill_fault_desc))) {
                LOG_CRITICAL(Debug, "FEX SIGILL host fault address classification: {}", ill_fault_desc);
            }
            uint64_t ill_accurate_guest_rip = 0;
            if (::AetherPS4::Fex::BachataReconstructAccurateGuestRIP(code_address, &ill_accurate_guest_rip)) {
                LOG_CRITICAL(Debug, "FEX SIGILL accurate guest rip at fault (reconstructed from host pc): {:#x}",
                            ill_accurate_guest_rip);
                if (auto* linker = Common::Singleton<Core::Linker>::Instance()) {
                    if (auto* module = linker->FindByAddress(ill_accurate_guest_rip)) {
                        LOG_CRITICAL(Debug,
                                    "FEX SIGILL guest rip is inside module '{}' (base={:#x}, offset={:#x})",
                                    module->name, module->GetBaseAddress(),
                                    ill_accurate_guest_rip - module->GetBaseAddress());
                    } else {
                        LOG_CRITICAL(Debug, "FEX SIGILL guest rip is not inside any loaded module");
                    }
                }
                if (auto* memory = Core::Memory::Instance()) {
                    ::Libraries::Kernel::OrbisVirtualQueryInfo ill_rip_vma{};
                    if (memory->TryVirtualQuery(ill_accurate_guest_rip, 0, &ill_rip_vma)) {
                        constexpr uint64_t kIllWindowBefore = 256;
                        constexpr uint64_t kIllWindowAfter = 256;
                        const auto ill_window_start = static_cast<uintptr_t>(ill_accurate_guest_rip - kIllWindowBefore);
                        static char ill_hex[2 * (kIllWindowBefore + kIllWindowAfter) + 1] = {};
                        char* w = ill_hex;
                        for (uint64_t i = 0; i < kIllWindowBefore + kIllWindowAfter; ++i) {
                            uint8_t byte = 0;
                            if (BachataSafeRead(ill_window_start + i, &byte)) {
                                w += std::snprintf(w, ill_hex + sizeof(ill_hex) - w, "%02x", byte);
                            } else {
                                w += std::snprintf(w, ill_hex + sizeof(ill_hex) - w, "??");
                            }
                        }
                        LOG_CRITICAL(Debug,
                                    "FEX SIGILL guest function window at accurate rip: rip={:#x} "
                                    "window_start={:#x} before={:#x} bytes={}",
                                    ill_accurate_guest_rip, ill_accurate_guest_rip - kIllWindowBefore,
                                    kIllWindowBefore, ill_hex);
                    }
                }
            }
            char ill_dispatcher_state[768] = {};
            if (::AetherPS4::Fex::BachataDumpDispatcherState(ill_dispatcher_state, sizeof(ill_dispatcher_state))) {
                LOG_CRITICAL(Debug, "FEX SIGILL dispatcher state: {}", ill_dispatcher_state);
            }
#ifdef __APPLE__
            {
                auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
                const auto lr = static_cast<uintptr_t>(arm_thread_state64_get_lr(apple_context->uc_mcontext->__ss));
                const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(apple_context->uc_mcontext->__ss));
                LOG_CRITICAL(Debug, "FEX SIGILL fault registers: pc={:#x} lr={:#x}", pc, lr);
                const auto& ts = apple_context->uc_mcontext->__ss;
                char ill_host_regs[512] = {};
                int ill_host_regs_len = 0;
                for (int i = 0; i < 29 && ill_host_regs_len < static_cast<int>(sizeof(ill_host_regs)) - 16;
                     i++) {
                    ill_host_regs_len += std::snprintf(ill_host_regs + ill_host_regs_len,
                                                   sizeof(ill_host_regs) - ill_host_regs_len, "x%d=%#llx ", i,
                                                   static_cast<unsigned long long>(ts.__x[i]));
                }
                LOG_CRITICAL(Debug, "FEX SIGILL host ARM64 registers at fault: {}",
                            std::string_view(ill_host_regs, ill_host_regs_len));
                uintptr_t ill_frame_fp = static_cast<uintptr_t>(arm_thread_state64_get_fp(ts));
                char ill_native_bt[1024] = {};
                int ill_native_bt_len = 0;
                for (int depth = 0; depth < 24 && ill_frame_fp != 0 &&
                     ill_native_bt_len < static_cast<int>(sizeof(ill_native_bt)) - 32;
                     depth++) {
                    if (ill_frame_fp < 0x1000 || (ill_frame_fp & 0x7) != 0) {
                        break;
                    }
                    uintptr_t saved_fp = 0;
                    uintptr_t saved_lr = 0;
                    if (!BachataSafeRead(ill_frame_fp, &saved_fp) ||
                        !BachataSafeRead(ill_frame_fp + sizeof(uintptr_t), &saved_lr)) {
                        break;
                    }
                    ill_native_bt_len += std::snprintf(ill_native_bt + ill_native_bt_len,
                                                   sizeof(ill_native_bt) - ill_native_bt_len, "[%d]=%#llx ",
                                                   depth, static_cast<unsigned long long>(saved_lr));
                    if (saved_lr == 0 || saved_fp == ill_frame_fp) {
                        break;
                    }
                    ill_frame_fp = saved_fp;
                }
                LOG_CRITICAL(Debug, "FEX SIGILL native ARM64 call stack (walk [fp]/[fp+8], symbolicate "
                                    "offline against this build's binary): {}",
                            std::string_view(ill_native_bt, ill_native_bt_len));
            }
#endif
#endif
            Common::Log::Flush();
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
            // This is the clearest, most direct proof available that StikDebug has stopped
            // responding: shadPS4 issued the BRK, expected it to be serviced, and it wasn't.
            // Recovering here keeps this one allocation from crashing the process outright, but
            // every JIT-capable operation for the rest of the session now has a debugger that
            // can't grant it memory -- see MarkStikDebugLikelyDead()'s own comment
            // (ios_jit_allocator.h) for what happens next.
            Core::IosJitAllocator::MarkStikDebugLikelyDead();
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
        // Diagnostic only: a GTA V session hit this fatal path repeatedly, on multiple unrelated
        // threads (RenderThread mid-qsort-callback, shadPS4:Main, Hang Detect Thread), all with
        // no preceding "ios_jit_allocator: requesting fresh execute-capable region" log line at
        // all -- meaning this specific SIGTRAP is NOT the BreakGetJITMapping BRK #0xf00d the
        // recovery path above targets (already confirmed thread-safe, see
        // g_expecting_jit_mapping_trap_count's own comment), it's something else entirely.
        // Dumping the actual trapping instruction's encoding here, rather than just the address,
        // settles what: BRK's own encoding is 0xD4200000 | (imm16 << 5), so a different imm16
        // than 0xf00d (or a word that doesn't even decode as BRK at all) narrows this down
        // immediately, without waiting on a full separate investigation cycle.
        // code_address is the fault PC itself, not a neighboring address like the dumps above --
        // the CPU just fetched and executed whatever's there to trap in the first place, so this
        // one read is inherently safe. BachataSafeRead anyway, purely for consistency with every
        // other read in this file now going through the same path.
        uint32_t trap_word = 0;
        BachataSafeRead(reinterpret_cast<uintptr_t>(code_address), &trap_word);
        const bool looks_like_brk = (trap_word & 0xFFE0001F) == 0xD4200000;
        const uint32_t brk_imm16 = (trap_word >> 5) & 0xFFFF;
        LOG_CRITICAL(Debug,
                    "FEX SIGTRAP raw instruction word at fault pc={:#x}: {:#010x} "
                    "is_brk_encoding={} brk_imm16={:#x}",
                    reinterpret_cast<uintptr_t>(code_address), trap_word, looks_like_brk, brk_imm16);
        // An earlier GTA V session hit this fatal path at four *different* addresses within one
        // continuous run, each occurring once, and a generalized "recover from any BRK" fix was
        // shipped for it -- WRONG, discovered later by symbolicating a session where that fix
        // let a game keep running (RenderThread mid-qsort-callback, several LOG lines of normal
        // activity) for a while after a recovered BRK, before dying anyway via an uncaught
        // "Terminate: Exception: Unreachable code" with none of Common::ReportCrash()'s usual
        // diagnostic detail. Root cause: assert.cpp's own Crash() -- the mechanism behind every
        // ASSERT/ASSERT_MSG/UNREACHABLE/UNREACHABLE_MSG failure anywhere in this entire codebase
        // -- is `__asm__ __volatile__("brk 0")` on ARM64. BRK #1 is separately the standard
        // compiler/libc trap immediate (__builtin_trap(), Swift/Obj-C runtime traps, abort()'s
        // own internal BRK, confirmed on an earlier session via a concurrent "Assertion failed:
        // ... atexit_register" on stderr). Recovering from either -- even just once, even before
        // the repeat-count safety net below would kick in -- means silently skipping over this
        // codebase's own "something is already badly wrong, stop now" signal (assert_fail_impl()
        // has *already* called Emulator::Instance()->Shutdown() by the time Crash() executes) and
        // letting the game keep running against a partially-torn-down emulator, which is worse
        // than the original crash, not a recovery from it. #0xf00d is the ONLY BRK immediate this
        // file has ever had a documented, protocol-level reason to consider skippable (StikDebug's
        // BreakpointJIT.framework, see ios_jit_allocator.h) -- recovery below is restricted to
        // exactly that value; every other BRK, including #0 and #1, falls straight through to the
        // fatal path, same as a real, unrecovered SIGTRAP always has.
        static std::atomic<uintptr_t> last_recovered_brk_address {0};
        static std::atomic<int> last_recovered_brk_consecutive_count {0};
        constexpr int kMaxConsecutiveRecoveries = 8;
        constexpr uint32_t kJitMappingBrkImm16 = 0xf00d;
        const auto this_address = reinterpret_cast<uintptr_t>(code_address);
        int consecutive_count = 1;
        if (last_recovered_brk_address.load(std::memory_order_relaxed) == this_address) {
            consecutive_count = last_recovered_brk_consecutive_count.fetch_add(1, std::memory_order_relaxed) + 1;
        } else {
            last_recovered_brk_address.store(this_address, std::memory_order_relaxed);
            last_recovered_brk_consecutive_count.store(1, std::memory_order_relaxed);
        }
        const bool is_recoverable_brk = looks_like_brk && brk_imm16 == kJitMappingBrkImm16;
        if (is_recoverable_brk && consecutive_count <= kMaxConsecutiveRecoveries) {
            LOG_CRITICAL(Debug,
                        "BACHATA_UNKNOWN_BRK_RECOVERED: BRK #{:#x} at {:#x} was not the expected "
                        "JIT-mapping call site (consecutive repeat #{}) -- likely a StikDebug-"
                        "internal breakpoint left armed after it went unresponsive; skipping it "
                        "instead of crashing",
                        brk_imm16, this_address, consecutive_count);
            // A stray StikDebug-internal breakpoint left armed after it went unresponsive is the
            // same underlying cause as the unserviced-JIT-mapping-BRK case above (see that call
            // site's comment) -- corroborating evidence, not a separate detector.
            Core::IosJitAllocator::MarkStikDebugLikelyDead();
            auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
            auto& ts = apple_context->uc_mcontext->__ss;
            const auto pc = static_cast<uintptr_t>(arm_thread_state64_get_pc(ts));
            arm_thread_state64_set_pc_fptr(ts, reinterpret_cast<void*>(pc + 4));
            return;
        }
        if (is_recoverable_brk) {
            LOG_CRITICAL(Debug,
                        "BACHATA_BRK_RECOVERY_ABANDONED: BRK #{:#x} at {:#x} has now repeated {} "
                        "times in a row -- letting it be fatal instead of looping forever",
                        brk_imm16, this_address, consecutive_count);
        } else if (looks_like_brk) {
            LOG_CRITICAL(Debug,
                        "BACHATA_BRK_NOT_RECOVERABLE: BRK #{:#x} at {:#x} is not the JIT-mapping "
                        "protocol's #0xf00d -- likely this codebase's own assert/unreachable "
                        "Crash() (#0) or a compiler/libc trap (#1); letting it be fatal so "
                        "Common::ReportCrash() below captures the real failure",
                        brk_imm16, this_address);
        }
        // Common::ReportCrash() is gated behind a BACHATA_CRASH_REGISTERS env var this build has
        // no way to set, so it silently no-ops here -- confirmed via a session where nothing at
        // all appeared between this point and UNREACHABLE_MSG's own log line below. When
        // brk_imm16 == 0 (this codebase's own assert.cpp Crash()), the LR at the point of the
        // trap is assert_fail_impl()'s own return address, i.e. this walk's [1] entry is exactly
        // which ASSERT/ASSERT_MSG/UNREACHABLE/UNREACHABLE_MSG call site actually fired --
        // needed since that's otherwise completely lost once we're back here re-executing
        // UNREACHABLE_MSG's own unrelated call to unreachable_impl().
        {
            auto* apple_context = reinterpret_cast<ucontext_t*>(raw_context);
            auto& ts = apple_context->uc_mcontext->__ss;
            uintptr_t frame_fp = static_cast<uintptr_t>(arm_thread_state64_get_fp(ts));
            const uintptr_t lr = static_cast<uintptr_t>(arm_thread_state64_get_lr(ts));
            char native_bt[1024] = {};
            int native_bt_len = std::snprintf(native_bt, sizeof(native_bt), "lr=%#llx ",
                                              static_cast<unsigned long long>(lr));
            for (int depth = 0; depth < 24 && frame_fp != 0 &&
                 native_bt_len < static_cast<int>(sizeof(native_bt)) - 32;
                 depth++) {
                if (frame_fp < 0x1000 || (frame_fp & 0x7) != 0) {
                    break;
                }
                uintptr_t saved_fp = 0;
                uintptr_t saved_lr = 0;
                if (!BachataSafeRead(frame_fp, &saved_fp) ||
                    !BachataSafeRead(frame_fp + sizeof(uintptr_t), &saved_lr)) {
                    break;
                }
                native_bt_len += std::snprintf(native_bt + native_bt_len,
                                               sizeof(native_bt) - native_bt_len, "[%d]=%#llx ",
                                               depth, static_cast<unsigned long long>(saved_lr));
                if (saved_lr == 0 || saved_fp == frame_fp) {
                    break;
                }
                frame_fp = saved_fp;
            }
            LOG_CRITICAL(Debug,
                        "BACHATA_SIGTRAP_NATIVE_STACK: (walk [fp]/[fp+8], symbolicate offline "
                        "against this build's binary): {}",
                        std::string_view(native_bt, native_bt_len));
        }
        {
            // Every native-stack address logged above sits somewhere in one of these images'
            // runtime range -- without this, addresses far outside the main executable's own
            // ~48MB __TEXT (as seen repeatedly for this exact crash) can't be attributed to a
            // specific system framework, embedded dylib, or the dyld shared cache at all. The
            // header pointer IS the image's actual runtime base address (it's mapped at the
            // very start of the image), so subtracting it from a stack address directly gives
            // a file offset symbolicatable against that image's own binary/dSYM offline --
            // no need to reason about link-time vmaddr vs. slide separately.
            // static, not a stack or heap buffer: a modern iOS process links well over a
            // thousand images (1385 observed on-device, mostly the dyld shared cache's own
            // frameworks), so this needs real room -- a stack array that size risks overrunning
            // a signal's alternate stack, and malloc/new here risks re-entering an allocator
            // that may itself be the very thing that just crashed (this exact crash has been
            // tracked into libsystem_malloc.dylib more than once). A previous, much smaller
            // buffer cut off after only 82 of the 1385 images, before ever reaching the base of
            // whichever image the persistent 0x1ce8xxxxxxx-family addresses (recurring across
            // this whole investigation) actually belong to.
            static char images_buf[131072];
            int images_len = 0;
            const uint32_t image_count = _dyld_image_count();
            for (uint32_t i = 0; i < image_count &&
                 images_len < static_cast<int>(sizeof(images_buf)) - 160;
                 i++) {
                const char* path = _dyld_get_image_name(i);
                const struct mach_header* header = _dyld_get_image_header(i);
                if (path == nullptr || header == nullptr) {
                    continue;
                }
                const char* base_name = std::strrchr(path, '/');
                base_name = base_name != nullptr ? base_name + 1 : path;
                images_len += std::snprintf(images_buf + images_len, sizeof(images_buf) - images_len,
                                            "[%u]=%s@%#llx ", i, base_name,
                                            static_cast<unsigned long long>(
                                                reinterpret_cast<uintptr_t>(header)));
            }
            LOG_CRITICAL(Debug,
                        "BACHATA_LOADED_IMAGES: ({} images total, name@runtime_base -- subtract "
                        "from a stack/fault address to get that image's own file offset): {}",
                        image_count, std::string_view(images_buf, images_len));
        }
        Common::ReportCrash(raw_context, sig, info);
        Common::Log::Flush();
        UNREACHABLE_MSG("Unhandled SIGTRAP at code address {} (not a JIT-mapping request, and not "
                        "a BRK instruction at all, or a BRK that repeated too many times to keep "
                        "recovering from)",
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
