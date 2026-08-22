// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) && defined(ARCH_ARM64)
#include <asm/sigcontext.h>
#include <sys/ucontext.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.pc;
#else
#error "Unsupported architecture"
#endif
}

void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__pc += length;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip += length;
#elif defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] += length;
#elif defined(__linux__) && defined(ARCH_ARM64)
    ((ucontext_t*)ctx)->uc_mcontext.pc += length;
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#elif defined(__linux__) && defined(ARCH_ARM64)
    const auto* context = static_cast<const ucontext_t*>(ctx);
    const auto* record = context->uc_mcontext.__reserved;
    const auto* const end = record + sizeof(context->uc_mcontext.__reserved);
    while (static_cast<size_t>(end - record) >= sizeof(_aarch64_ctx)) {
        const auto* header = reinterpret_cast<const _aarch64_ctx*>(record);
        if (header->magic == 0 && header->size == 0) {
            break;
        }
        const size_t remaining = end - record;
        if (header->size < sizeof(*header) || header->size > remaining ||
            (header->size % alignof(_aarch64_ctx)) != 0) {
            return false;
        }
        if (header->magic == ESR_MAGIC) {
            if (header->size < sizeof(esr_context)) {
                return false;
            }
            const auto* esr = reinterpret_cast<const esr_context*>(record);
            return esr->esr & 0x40;
        }
        record += header->size;
    }
    return false;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
