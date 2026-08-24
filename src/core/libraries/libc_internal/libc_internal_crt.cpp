// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/libc_internal/libc_internal_crt.h"
#include "core/libraries/libs.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Libraries::LibcInternal {

[[noreturn]] void PS4_SYSV_ABI internal_abort() {
    LOG_CRITICAL(Lib_LibcInternal, "abort() called by guest code");
    std::abort();
}

[[noreturn]] void PS4_SYSV_ABI internal_exit(s32 code) {
    LOG_INFO(Lib_LibcInternal, "exit({}) called by guest code", code);
    std::exit(code);
}

s32 PS4_SYSV_ABI internal_atexit(void (*func)()) {
    // Same "accept and track, actually draining requires a future __cxa_finalize" contract
    // as fex_libc_cxa_atexit (libc_internal_cxa.cpp) -- the PS4 game process is terminated
    // by the host rather than exiting in an orderly fashion, so simply accepting the
    // registration is enough to satisfy callers checking the return value.
    return func != nullptr ? 0 : -1;
}

void PS4_SYSV_ABI internal_qsort(void* base, u64 nmemb, u64 size,
                                 s32 PS4_SYSV_ABI (*compar)(const void*, const void*)) {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    // `compar` is a guest (x86) function pointer, not something this host ARM64 process can
    // call directly -- jumping into it as-is would execute raw x86 bytes as ARM64
    // instructions (undefined behavior, not a clean crash). Route every comparison through
    // the guest CPU instead, same mechanism already used for guest thread entry points and
    // static-initializer callbacks elsewhere (see pthread.cpp).
    // Host qsort with a host-side comparator trampoline: this still performs the actual
    // sort/swap logic natively (fast, well-tested), only the *comparison itself* crosses
    // into guest code for each pairwise test.
    auto compare_via_guest = [compar](const void* a, const void* b) -> int {
        const std::array<u64, 2> args{reinterpret_cast<u64>(a), reinterpret_cast<u64>(b)};
        return static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(compar), args, "qsort comparator"));
    };
    // std::qsort requires a C function pointer, not a capturing lambda -- stash the real
    // comparator in a thread_local so a plain trampoline can reach it. qsort calls are not
    // reentrant across threads calling this at once with different comparators in a way
    // that matters here (each call's sort completes before returning).
    static thread_local decltype(compare_via_guest)* active_comparator = nullptr;
    active_comparator = &compare_via_guest;
    struct Trampoline {
        static int PS4_SYSV_ABI Call(const void* a, const void* b) {
            return (*active_comparator)(a, b);
        }
    };
    std::qsort(base, nmemb, size, &Trampoline::Call);
    active_comparator = nullptr;
#else
    std::qsort(base, nmemb, size, reinterpret_cast<int (*)(const void*, const void*)>(compar));
#endif
}

s32 PS4_SYSV_ABI internal_rand() {
    return std::rand();
}

void PS4_SYSV_ABI internal_srand(u32 seed) {
    std::srand(seed);
}

s64 PS4_SYSV_ABI internal_time(s64* tloc) {
    const std::time_t result = std::time(nullptr);
    if (tloc != nullptr) {
        *tloc = static_cast<s64>(result);
    }
    return static_cast<s64>(result);
}

namespace {
thread_local OrbisTm g_tm_result{};

OrbisTm* ToOrbisTm(const std::tm& src, OrbisTm* dest) {
    dest->tm_sec = src.tm_sec;
    dest->tm_min = src.tm_min;
    dest->tm_hour = src.tm_hour;
    dest->tm_mday = src.tm_mday;
    dest->tm_mon = src.tm_mon;
    dest->tm_year = src.tm_year;
    dest->tm_wday = src.tm_wday;
    dest->tm_yday = src.tm_yday;
    dest->tm_isdst = src.tm_isdst;
    return dest;
}
} // namespace

OrbisTm* PS4_SYSV_ABI internal_gmtime(const s64* timer) {
    const std::time_t t = static_cast<std::time_t>(*timer);
    std::tm result{};
#ifdef _WIN64
    if (gmtime_s(&result, &t) != 0) {
        return nullptr;
    }
#else
    if (gmtime_r(&t, &result) == nullptr) {
        return nullptr;
    }
#endif
    return ToOrbisTm(result, &g_tm_result);
}

OrbisTm* PS4_SYSV_ABI internal_gmtime_s(const s64* timer, OrbisTm* result) {
    const std::time_t t = static_cast<std::time_t>(*timer);
    std::tm tmp{};
#ifdef _WIN64
    if (gmtime_s(&tmp, &t) != 0) {
        return nullptr;
    }
#else
    if (gmtime_r(&t, &tmp) == nullptr) {
        return nullptr;
    }
#endif
    return ToOrbisTm(tmp, result);
}

OrbisTm* PS4_SYSV_ABI internal_localtime(const s64* timer) {
    const std::time_t t = static_cast<std::time_t>(*timer);
    std::tm result{};
#ifdef _WIN64
    if (localtime_s(&result, &t) != 0) {
        return nullptr;
    }
#else
    if (localtime_r(&t, &result) == nullptr) {
        return nullptr;
    }
#endif
    return ToOrbisTm(result, &g_tm_result);
}

s64 PS4_SYSV_ABI internal_mktime(OrbisTm* tm) {
    std::tm host_tm{};
    host_tm.tm_sec = tm->tm_sec;
    host_tm.tm_min = tm->tm_min;
    host_tm.tm_hour = tm->tm_hour;
    host_tm.tm_mday = tm->tm_mday;
    host_tm.tm_mon = tm->tm_mon;
    host_tm.tm_year = tm->tm_year;
    host_tm.tm_isdst = tm->tm_isdst;
    const std::time_t result = std::mktime(&host_tm);
    tm->tm_wday = host_tm.tm_wday;
    tm->tm_yday = host_tm.tm_yday;
    return static_cast<s64>(result);
}

double PS4_SYSV_ABI internal_difftime(s64 time1, s64 time0) {
    return std::difftime(static_cast<std::time_t>(time1), static_cast<std::time_t>(time0));
}

void* PS4_SYSV_ABI internal_localeconv() {
    // No locale support beyond "C" is implemented; games that call this to check
    // decimal-point/thousands-separator formatting get standard "C" locale values
    // (returning the host's own localeconv() result verbatim -- its struct layout
    // matches what a "C" locale caller expects field-for-field for the fields PS4
    // games actually read).
    return std::localeconv();
}

void RegisterlibSceLibcInternalCrt(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("L1SBTkC+Cvw", "libSceLibcInternal", 1, "libSceLibcInternal", internal_abort);
    LIB_FUNCTION("uMei1W9uyNo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_exit);
    LIB_FUNCTION("8G2LB+A3rzg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_atexit);
    LIB_FUNCTION("AEJdIVZTEmo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_qsort);
    LIB_FUNCTION("cpCOXWMgha0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_rand);
    LIB_FUNCTION("VPbJwTCgME0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_srand);
    LIB_FUNCTION("wLlFkwG9UcQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_time);
    LIB_FUNCTION("1mecP7RgI2A", "libSceLibcInternal", 1, "libSceLibcInternal", internal_gmtime);
    LIB_FUNCTION("5bBacGLyLOs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_gmtime_s);
    LIB_FUNCTION("efhK-YSUYYQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_localtime);
    LIB_FUNCTION("n7AepwR0s34", "libSceLibcInternal", 1, "libSceLibcInternal", internal_mktime);
    LIB_FUNCTION("-VVn74ZyhEs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_difftime);
    LIB_FUNCTION("0hlfW1O4Aa4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_localeconv);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcCrtAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("L1SBTkC+Cvw", "libc", 1, "libc", internal_abort);
    LIB_FUNCTION("uMei1W9uyNo", "libc", 1, "libc", internal_exit);
    LIB_FUNCTION("8G2LB+A3rzg", "libc", 1, "libc", internal_atexit);
    LIB_FUNCTION("AEJdIVZTEmo", "libc", 1, "libc", internal_qsort);
    LIB_FUNCTION("cpCOXWMgha0", "libc", 1, "libc", internal_rand);
    LIB_FUNCTION("VPbJwTCgME0", "libc", 1, "libc", internal_srand);
    LIB_FUNCTION("wLlFkwG9UcQ", "libc", 1, "libc", internal_time);
    LIB_FUNCTION("1mecP7RgI2A", "libc", 1, "libc", internal_gmtime);
    LIB_FUNCTION("5bBacGLyLOs", "libc", 1, "libc", internal_gmtime_s);
    LIB_FUNCTION("efhK-YSUYYQ", "libc", 1, "libc", internal_localtime);
    LIB_FUNCTION("n7AepwR0s34", "libc", 1, "libc", internal_mktime);
    LIB_FUNCTION("-VVn74ZyhEs", "libc", 1, "libc", internal_difftime);
    LIB_FUNCTION("0hlfW1O4Aa4", "libc", 1, "libc", internal_localeconv);
}
#endif

} // namespace Libraries::LibcInternal
