// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
void* PS4_SYSV_ABI internal_localeconv();

void RegisterlibSceLibcInternalCrt(Core::Loader::SymbolsResolver* sym);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcCrtAliases(Core::Loader::SymbolsResolver* sym);
#endif

} // namespace Libraries::LibcInternal
