// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/libc_internal/libc_internal_cxa.h"
#include "core/libraries/libs.h"

namespace Libraries::LibcInternal {
namespace {

std::mutex GuardMutex;
std::condition_variable GuardCondition;
std::unordered_map<u64*, std::thread::id> GuardOwners;

/*
 * __cxa_atexit registrations. A C++ guest registers static-object destructors
 * through this entry point with (func, arg, dso_handle). Per the Itanium C++
 * ABI the callbacks for a DSO run in reverse registration order when
 * __cxa_finalize(dso_handle) is called or at process exit. On the PS4 the
 * game process is terminated by the host (no orderly exit), so simply
 * accepting the registration is enough to satisfy module start; we keep the
 * table so a future __cxa_finalize implementation can drain it.
 */
struct CxaAtexitEntry {
    void (*func)(void*);
    void* arg;
    void* dso_handle;
};
std::mutex AtexitMutex;
std::vector<CxaAtexitEntry> AtexitEntries;

u8* GuardBytes(u64* guard_object) {
    return reinterpret_cast<u8*>(guard_object);
}

bool IsInitialized(u64* guard_object) {
    return std::atomic_ref<u8>{GuardBytes(guard_object)[0]}.load(std::memory_order_acquire) != 0;
}

} // namespace

int PS4_SYSV_ABI fex_libc_cxa_guard_acquire(u64* guard_object) {
    std::unique_lock lock{GuardMutex};
    for (;;) {
        if (IsInitialized(guard_object)) {
            return 0;
        }

        const auto owner = GuardOwners.find(guard_object);
        if (owner == GuardOwners.end()) {
            GuardOwners.emplace(guard_object, std::this_thread::get_id());
            GuardBytes(guard_object)[1] = 1;
            return 1;
        }
        if (owner->second == std::this_thread::get_id()) {
            LOG_CRITICAL(Lib_LibcInternal,
                         "recursive initialization of guest static at {}",
                         static_cast<void*>(guard_object));
            std::terminate();
        }

        GuardCondition.wait(lock);
    }
}

void PS4_SYSV_ABI fex_libc_cxa_guard_release(u64* guard_object) {
    {
        std::lock_guard lock{GuardMutex};
        std::atomic_ref<u8>{GuardBytes(guard_object)[0]}.store(1, std::memory_order_release);
        GuardBytes(guard_object)[1] = 0;
        if (GuardOwners.erase(guard_object) != 1) {
            LOG_ERROR(Lib_LibcInternal, "release of unowned guest static guard at {}",
                      static_cast<void*>(guard_object));
        }
    }
    GuardCondition.notify_all();
}

void PS4_SYSV_ABI fex_libc_cxa_guard_abort(u64* guard_object) {
    {
        std::lock_guard lock{GuardMutex};
        GuardBytes(guard_object)[1] = 0;
        if (GuardOwners.erase(guard_object) != 1) {
            LOG_ERROR(Lib_LibcInternal, "abort of unowned guest static guard at {}",
                      static_cast<void*>(guard_object));
        }
    }
    GuardCondition.notify_all();
}

int PS4_SYSV_ABI fex_libc_cxa_atexit(void (*func)(void*), void* arg, void* dso_handle) {
    /* __cxa_atexit is invoked by C++ static-object initializers in guest
     * modules to register their destructor. Return 0 to accept the
     * registration; the table is retained for a future __cxa_finalize. */
    if (func == nullptr) {
        return -1;
    }
    {
        std::lock_guard lock{AtexitMutex};
        AtexitEntries.push_back({func, arg, dso_handle});
    }
    LOG_TRACE(Lib_LibcInternal, "registered __cxa_atexit func={} arg={} dso={}",
              fmt::ptr(reinterpret_cast<void*>(func)), arg, dso_handle);
    return 0;
}

void RegisterFexLibcCxaAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3GPpjQdAMTw", "libc", 1, "libc", fex_libc_cxa_guard_acquire);
    LIB_FUNCTION("9rAeANT2tyE", "libc", 1, "libc", fex_libc_cxa_guard_release);
    LIB_FUNCTION("2emaaluWzUw", "libc", 1, "libc", fex_libc_cxa_guard_abort);
    // FEX adapter looks up C++ runtime symbols under libSceLibcInternal.
    LIB_FUNCTION("3GPpjQdAMTw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 fex_libc_cxa_guard_acquire);
    LIB_FUNCTION("9rAeANT2tyE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 fex_libc_cxa_guard_release);
    LIB_FUNCTION("2emaaluWzUw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 fex_libc_cxa_guard_abort);
    LIB_FUNCTION("tsvEmnenz48", "libSceLibcInternal", 1, "libSceLibcInternal",
                 fex_libc_cxa_atexit);
}

} // namespace Libraries::LibcInternal
