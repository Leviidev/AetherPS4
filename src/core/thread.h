// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {
struct PthreadAttr;
} // namespace Libraries::Kernel

namespace Core {
#ifdef WIN32
using ThreadFunc = DWORD (*)(void*);
#else
using ThreadFunc = void* (*)(void*);
#endif

class NativeThread {
public:
    NativeThread();
    ~NativeThread();

    int Create(ThreadFunc func, void* arg);

    /**
     * Tears down everything this object itself owns (the alternate signal stack) --
     * everything Exit() needs that reads from `this`. Must run before this thread becomes
     * visible as terminated to any other thread (see ExitThread()'s own comment in
     * pthread.cpp): once that happens, another thread can reap and reuse this object's
     * pooled memory at any moment, and Exit()'s own reads of sig_stack_ptr/native_handle
     * would then be racing a use-after-free.
     */
    void PrepareExit();

    /// The actual, real, never-returning thread exit. Reads no member state of `this` (see
    /// PrepareExit()'s own comment) precisely so it stays safe to call after this object's
    /// memory may already have been reclaimed by another thread.
    void Exit();

    void Initialize();

    uintptr_t GetHandle() {
        return reinterpret_cast<uintptr_t>(native_handle);
    }

    u64 GetTid() {
        return tid;
    }

private:
#ifdef _WIN64
    void* native_handle;
#else
    uintptr_t native_handle;
    void* sig_stack_ptr = nullptr;
#endif
    u64 tid;
};

} // namespace Core