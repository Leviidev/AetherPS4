// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_str.h"

namespace Libraries::LibcInternal {

s32 PS4_SYSV_ABI internal_strcpy_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcpy_s(dest, dest_size, src);
#else
    std::strcpy(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcat_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcat_s(dest, dest_size, src);
#else
    std::strcat(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcmp(const char* str1, const char* str2) {
    return std::strcmp(str1, str2);
}

s32 PS4_SYSV_ABI internal_strncmp(const char* str1, const char* str2, size_t num) {
    return std::strncmp(str1, str2, num);
}

char* PS4_SYSV_ABI internal_strcpy(char* dest, const char* src) {
    LOG_DEBUG(Lib_LibcInternal, "internal_strcpy: dest={} src={}", static_cast<const void*>(dest),
              static_cast<const void*>(src));
    return std::strcpy(dest, src);
}

size_t PS4_SYSV_ABI internal_strlen(const char* str) {
    LOG_DEBUG(Lib_LibcInternal, "internal_strlen: str={}", static_cast<const void*>(str));
    return std::strlen(str);
}

size_t PS4_SYSV_ABI internal_wcslen(const u16* str) {
    LOG_DEBUG(Lib_LibcInternal, "internal_wcslen: str={}", static_cast<const void*>(str));
    const u16* end = str;
    while (*end != 0) {
        ++end;
    }
    return static_cast<size_t>(end - str);
}

char* PS4_SYSV_ABI internal_strncpy(char* dest, const char* src, std::size_t count) {
    // No logging here at all previously -- confirmed on-device this call (or one of its
    // immediate neighbors in a tight wcslen/strlen/strcpy loop, none of which log either) is
    // exactly where Rocket League crashes reading a wild pointer, right after the FIOS2 gating
    // fix let it progress past its earlier "package Core" blocker. Log the raw pointer values
    // before touching them so the next crash log shows exactly which one was bad, rather than
    // just "somewhere in this call".
    LOG_DEBUG(Lib_LibcInternal, "internal_strncpy: dest={} src={} count={}",
              static_cast<const void*>(dest), static_cast<const void*>(src), count);
    return std::strncpy(dest, src, count);
}

s32 PS4_SYSV_ABI internal_strncpy_s(char* dest, size_t destsz, const char* src, size_t count) {
#ifdef _WIN64
    return strncpy_s(dest, destsz, src, count);
#else
    std::strcpy(dest, src);
    return 0;
#endif
}

char* PS4_SYSV_ABI internal_strcat(char* dest, const char* src) {
    return std::strcat(dest, src);
}

const char* PS4_SYSV_ABI internal_strchr(const char* str, int c) {
    return std::strchr(str, c);
}

void RegisterlibSceLibcInternalStr(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("5Xa2ACNECdo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcpy_s);
    LIB_FUNCTION("K+gcnFFJKVc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat_s);
    LIB_FUNCTION("Ovb2dSJOAuE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcmp);
    LIB_FUNCTION("aesyjrHVWy4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncmp);
    LIB_FUNCTION("j4ViWNHEgww", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strlen);
    LIB_FUNCTION("6sJWiWSRuqk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy);
    LIB_FUNCTION("YNzNkJzYqEg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy_s);
    LIB_FUNCTION("Ls4tzzhimqQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat);
    LIB_FUNCTION("ob5xAW4ln-0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strchr);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcStrAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("kiZSXIWd9vg", "libc", 1, "libc", internal_strcpy);
    LIB_FUNCTION("Ls4tzzhimqQ", "libc", 1, "libc", internal_strcat);
    LIB_FUNCTION("j4ViWNHEgww", "libc", 1, "libc", internal_strlen);
    LIB_FUNCTION("WkkeywLJcgU", "libc", 1, "libc", internal_wcslen);
    LIB_FUNCTION("Ovb2dSJOAuE", "libc", 1, "libc", internal_strcmp);
}
#endif

} // namespace Libraries::LibcInternal
