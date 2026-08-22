// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/sigsys_trap.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <unistd.h>

#ifdef __APPLE__
// Darwin's <ucontext.h> hard-errors on the deprecated getcontext/setcontext/swapcontext
// declarations unless _XOPEN_SOURCE is defined; only the ucontext_t/mcontext_t *types*
// are needed below, never those functions. pc/sp/fp/lr are pointer-authentication-opaque
// fields on Apple Silicon requiring the arm_thread_state64_get_* accessor macros.
#define _XOPEN_SOURCE 1
#include <ucontext.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#endif

namespace Common {

namespace {
struct sigaction g_old_sigsys_action;
// Alternate signal stack so the handler can run even if the crashing thread's
// stack is exhausted. SA_ONSTACK (set below) routes the signal here.
std::array<unsigned char, 65536> g_sigsys_altstack alignas(16){};

void BachataSigsysHandler(int signo, siginfo_t* info, void* uctx) {
    ucontext_t* _ctx = reinterpret_cast<ucontext_t*>(uctx);
    uint64_t pc = 0, sp = 0, x8 = 0;
    uint64_t x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x29 = 0, x30 = 0;
#ifdef __aarch64__
    if (_ctx) {
#ifdef __APPLE__
        const auto& ts = _ctx->uc_mcontext->__ss;
        pc = (uint64_t)arm_thread_state64_get_pc(ts);
        sp = (uint64_t)arm_thread_state64_get_sp(ts);
        x8 = ts.__x[8];
        x0 = ts.__x[0];
        x1 = ts.__x[1];
        x2 = ts.__x[2];
        x3 = ts.__x[3];
        x4 = ts.__x[4];
        x5 = ts.__x[5];
        x29 = (uint64_t)arm_thread_state64_get_fp(ts);
        x30 = (uint64_t)arm_thread_state64_get_lr(ts);
#else
        pc = _ctx->uc_mcontext.pc;
        sp = _ctx->uc_mcontext.sp;
        x8 = _ctx->uc_mcontext.regs[8];
        x0 = _ctx->uc_mcontext.regs[0];
        x1 = _ctx->uc_mcontext.regs[1];
        x2 = _ctx->uc_mcontext.regs[2];
        x3 = _ctx->uc_mcontext.regs[3];
        x4 = _ctx->uc_mcontext.regs[4];
        x5 = _ctx->uc_mcontext.regs[5];
        x29 = _ctx->uc_mcontext.regs[29];
        x30 = _ctx->uc_mcontext.regs[30];
#endif
    }
#endif

    // Best-effort guest RIP/syscall capture. On the FEX guest CPU path, mid-JIT
    // guest state lives in SRA host regs, but at a host syscall boundary the
    // CurrentFrame holds the spilled guest RIP and RAX. Returns false (leaves
    // guest_rip/guest_syscall as the "unavailable" sentinels) when no FEX thread
    // is active (e.g. crash during host-only init or in a non-FEX host library).
    uint64_t guest_rip = 0;
    uint64_t guest_syscall = 0;
    bool have_guest = false;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    have_guest = AetherPS4::Fex::BachataQueryGuestRipSyscall(&guest_rip, &guest_syscall);
#endif

#ifdef __APPLE__
    uint64_t apple_tid = 0;
    pthread_threadid_np(nullptr, &apple_tid);
#endif

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "[Bachata.FEX.SIGSYS] signo=%d\n"
        "[Bachata.FEX.SIGSYS] code=%d\n"
        "[Bachata.FEX.SIGSYS] errno=%d\n"
        "[Bachata.FEX.SIGSYS] syscall=%d\n"
        "[Bachata.FEX.SIGSYS] arch=0x%x\n"
        "[Bachata.FEX.SIGSYS] call_addr=0x%lx\n"
        "[Bachata.FEX.SIGSYS] host_pc=0x%lx\n"
        "[Bachata.FEX.SIGSYS] host_x8=%lu\n"
        "[Bachata.FEX.SIGSYS] guest_rip=0x%lx\n"
        "[Bachata.FEX.SIGSYS] guest_syscall=%s%lu\n"
        "[Bachata.FEX.SIGSYS] host_sp=0x%lx host_x0=0x%lx host_x1=0x%lx host_x2=0x%lx host_x3=0x%lx host_x4=0x%lx host_x5=0x%lx host_x29=0x%lx host_x30=0x%lx pid=%d tid=%ld\n",
        info ? info->si_signo : signo,
        info ? info->si_code : 0,
        info ? info->si_errno : 0,
#ifdef __APPLE__
        // si_syscall/si_arch/si_call_addr are populated by Linux's seccomp-based syscall
        // filtering, which SIGSYS is normally paired with there; Darwin has no seccomp
        // equivalent and siginfo_t carries none of these fields, so there's nothing
        // meaningful to report here.
        -1,
        0u,
        0UL,
#else
        info ? info->si_syscall : -1,
        info ? info->si_arch : 0,
        info ? (unsigned long)(uintptr_t)info->si_call_addr : 0UL,
#endif
        (unsigned long)pc,
        (unsigned long)x8,
        (unsigned long)guest_rip,
        have_guest ? "" : "unavailable ",
        (unsigned long)guest_syscall,
        (unsigned long)sp, (unsigned long)x0, (unsigned long)x1, (unsigned long)x2,
        (unsigned long)x3, (unsigned long)x4, (unsigned long)x5, (unsigned long)x29,
        (unsigned long)x30, ::getpid(),
#ifdef __APPLE__
        (long)apple_tid);
#else
        (long)::syscall(SYS_gettid));
#endif

    if (len > 0) {
        ::write(STDERR_FILENO, buf, static_cast<size_t>(len));
    }

    if (g_old_sigsys_action.sa_flags & SA_SIGINFO) {
        if (g_old_sigsys_action.sa_sigaction) {
            g_old_sigsys_action.sa_sigaction(signo, info, uctx);
        }
    } else if (g_old_sigsys_action.sa_handler != SIG_DFL && g_old_sigsys_action.sa_handler != SIG_IGN) {
        g_old_sigsys_action.sa_handler(signo);
    } else {
        signal(signo, SIG_DFL);
        raise(signo);
    }
}
} // namespace

void InstallBachataSigsysTrap() {
    // Install an alternate stack first: SA_ONSTACK redirects the signal here so
    // the handler runs even on an exhausted main stack.
    stack_t ss{};
    ss.ss_sp = g_sigsys_altstack.data();
    ss.ss_size = g_sigsys_altstack.size();
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = BachataSigsysHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, &g_old_sigsys_action);
}

} // namespace Common
