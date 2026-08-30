// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::LibcInternal {

struct OrbisTm {
    s32 tm_sec;
    s32 tm_min;
    s32 tm_hour;
    s32 tm_mday;
    s32 tm_mon;
    s32 tm_year;
    s32 tm_wday;
    s32 tm_yday;
    s32 tm_isdst;
};

// Sony's Dinkumware-derived libc extends the ISO C lconv prefix with wide-string
// counterparts. Darwin instead places six additional ISO/POSIX flag bytes after
// n_sign_posn, so returning the host structure directly makes guest code read those
// 0x7f bytes as _W_decimal_point (0x7f7f7f7f7f7f on iOS).
struct OrbisLconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    u32* wide_decimal_point;
    u32* wide_thousands_sep;
    u32* wide_int_curr_symbol;
    u32* wide_currency_symbol;
    u32* wide_mon_decimal_point;
    u32* wide_mon_thousands_sep;
    u32* wide_positive_sign;
    u32* wide_negative_sign;
};
static_assert(offsetof(OrbisLconv, wide_decimal_point) == 88);
static_assert(sizeof(OrbisLconv) == 152);

[[noreturn]] void PS4_SYSV_ABI internal_abort();
[[noreturn]] void PS4_SYSV_ABI internal_exit(s32 code);
s32 PS4_SYSV_ABI internal_atexit(void (*func)());
void PS4_SYSV_ABI internal_qsort(void* base, u64 nmemb, u64 size,
                                 s32 PS4_SYSV_ABI (*compar)(const void*, const void*));
s32 PS4_SYSV_ABI internal_rand();
void PS4_SYSV_ABI internal_srand(u32 seed);
s64 PS4_SYSV_ABI internal_time(s64* tloc);
OrbisTm* PS4_SYSV_ABI internal_gmtime(const s64* timer);
OrbisTm* PS4_SYSV_ABI internal_gmtime_s(const s64* timer, OrbisTm* result);
OrbisTm* PS4_SYSV_ABI internal_localtime(const s64* timer);
s64 PS4_SYSV_ABI internal_mktime(OrbisTm* tm);
double PS4_SYSV_ABI internal_difftime(s64 time1, s64 time0);
OrbisLconv* PS4_SYSV_ABI internal_localeconv();

void RegisterlibSceLibcInternalCrt(Core::Loader::SymbolsResolver* sym);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcCrtAliases(Core::Loader::SymbolsResolver* sym);
#endif

} // namespace Libraries::LibcInternal
