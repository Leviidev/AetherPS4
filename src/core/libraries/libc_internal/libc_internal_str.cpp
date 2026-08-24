// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <strings.h>

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
    return std::strcpy(dest, src);
}

size_t PS4_SYSV_ABI internal_strlen(const char* str) {
    return std::strlen(str);
}

size_t PS4_SYSV_ABI internal_wcslen(const u16* str) {
    const u16* end = str;
    while (*end != 0) {
        ++end;
    }
    return static_cast<size_t>(end - str);
}

char* PS4_SYSV_ABI internal_strncpy(char* dest, const char* src, std::size_t count) {
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

s32 PS4_SYSV_ABI internal_strcasecmp(const char* str1, const char* str2) {
    return ::strcasecmp(str1, str2);
}

size_t PS4_SYSV_ABI internal_strcspn(const char* str1, const char* str2) {
    return std::strcspn(str1, str2);
}

char* PS4_SYSV_ABI internal_strncat(char* dest, const char* src, size_t count) {
    return std::strncat(dest, src, count);
}

char* PS4_SYSV_ABI internal_strrchr(const char* str, int c) {
    return const_cast<char*>(std::strrchr(str, c));
}

size_t PS4_SYSV_ABI internal_strspn(const char* str1, const char* str2) {
    return std::strspn(str1, str2);
}

char* PS4_SYSV_ABI internal_strstr(const char* str1, const char* str2) {
    return const_cast<char*>(std::strstr(str1, str2));
}

char* PS4_SYSV_ABI internal_strtok(char* str, const char* delim) {
    return std::strtok(str, delim);
}

long PS4_SYSV_ABI internal_strtol(const char* str, char** endptr, int base) {
    return std::strtol(str, endptr, base);
}

char* PS4_SYSV_ABI internal_strerror(int errnum) {
    return std::strerror(errnum);
}

// PS4's wchar_t is 16 bits (like Windows), unlike this host's 32-bit wchar_t, so these
// operate directly on u16 code units rather than going through host wcs*() functions --
// reinterpret_casting a u16 buffer to this platform's wchar_t* would read/write the wrong
// element size and corrupt adjacent memory. Matches internal_wcslen's existing approach
// below (manual loop, no host wchar_t involved).

u16* PS4_SYSV_ABI internal_wcscat(u16* dest, const u16* src) {
    u16* end = dest;
    while (*end != 0) {
        ++end;
    }
    while ((*end++ = *src++) != 0) {
    }
    return dest;
}

u16* PS4_SYSV_ABI internal_wcschr(const u16* str, u16 c) {
    while (*str != 0) {
        if (*str == c) {
            return const_cast<u16*>(str);
        }
        ++str;
    }
    return c == 0 ? const_cast<u16*>(str) : nullptr;
}

s32 PS4_SYSV_ABI internal_wcscmp(const u16* str1, const u16* str2) {
    while (*str1 != 0 && *str1 == *str2) {
        ++str1;
        ++str2;
    }
    return static_cast<s32>(*str1) - static_cast<s32>(*str2);
}

u16* PS4_SYSV_ABI internal_wcscpy(u16* dest, const u16* src) {
    u16* orig_dest = dest;
    while ((*dest++ = *src++) != 0) {
    }
    return orig_dest;
}

s32 PS4_SYSV_ABI internal_wcsncmp(const u16* str1, const u16* str2, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (str1[i] != str2[i] || str1[i] == 0) {
            return static_cast<s32>(str1[i]) - static_cast<s32>(str2[i]);
        }
    }
    return 0;
}

u16* PS4_SYSV_ABI internal_wcsncpy(u16* dest, const u16* src, size_t count) {
    size_t i = 0;
    for (; i < count && src[i] != 0; ++i) {
        dest[i] = src[i];
    }
    for (; i < count; ++i) {
        dest[i] = 0;
    }
    return dest;
}

u16* PS4_SYSV_ABI internal_wcsrchr(const u16* str, u16 c) {
    const u16* last = nullptr;
    for (; *str != 0; ++str) {
        if (*str == c) {
            last = str;
        }
    }
    if (c == 0) {
        return const_cast<u16*>(str);
    }
    return const_cast<u16*>(last);
}

u16* PS4_SYSV_ABI internal_wcsstr(const u16* str1, const u16* str2) {
    if (*str2 == 0) {
        return const_cast<u16*>(str1);
    }
    for (; *str1 != 0; ++str1) {
        const u16* a = str1;
        const u16* b = str2;
        while (*a != 0 && *b != 0 && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0) {
            return const_cast<u16*>(str1);
        }
    }
    return nullptr;
}

u64 PS4_SYSV_ABI internal_wcstoull(const u16* str, u16** endptr, int base) {
    // Narrow to a temporary ASCII buffer for std::strtoull's parsing -- the numeric
    // formats this is used for (decimal/hex integers) are always within the ASCII range.
    char narrow[64] = {};
    size_t i = 0;
    for (; i < std::size(narrow) - 1 && str[i] != 0; ++i) {
        narrow[i] = static_cast<char>(str[i]);
    }
    narrow[i] = '\0';
    char* narrow_end = nullptr;
    const u64 result = std::strtoull(narrow, &narrow_end, base);
    if (endptr) {
        *endptr = const_cast<u16*>(str) + (narrow_end - narrow);
    }
    return result;
}

size_t PS4_SYSV_ABI internal_wcsrtombs(char* dest, const u16** src, size_t len, void* ps) {
    // ASCII-range-only conversion (matches internal_wcstoull's narrowing approach): PS4
    // SDK text needing this is overwhelmingly ASCII, and implementing true UTF-16 to
    // guest-locale multibyte conversion here would need locale/codepage support this
    // codebase doesn't otherwise have.
    const u16* s = *src;
    size_t written = 0;
    while (written < len) {
        if (*s == 0) {
            if (dest) {
                dest[written] = '\0';
            }
            *src = nullptr;
            return written;
        }
        if (dest) {
            dest[written] = static_cast<char>(*s);
        }
        ++s;
        ++written;
    }
    *src = s;
    return written;
}

size_t PS4_SYSV_ABI internal_mbsrtowcs(u16* dest, const char** src, size_t len, void* ps) {
    const char* s = *src;
    size_t written = 0;
    while (!dest || written < len) {
        if (*s == '\0') {
            if (dest) {
                dest[written] = 0;
            }
            *src = nullptr;
            return written;
        }
        if (dest) {
            dest[written] = static_cast<u16>(static_cast<unsigned char>(*s));
        }
        ++s;
        ++written;
    }
    *src = s;
    return written;
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
    LIB_FUNCTION("AV6ipCNa4Rw", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcasecmp);
    LIB_FUNCTION("q0F6yS-rCms", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcspn);
    LIB_FUNCTION("kHg45qPC6f0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncat);
    LIB_FUNCTION("9yDWMxEFdJU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strrchr);
    LIB_FUNCTION("-kU6bB4M-+k", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strspn);
    LIB_FUNCTION("viiwFMaNamA", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strstr);
    LIB_FUNCTION("oVkZ8W8-Q8A", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strtok);
    LIB_FUNCTION("mXlxhmLNMPg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strtol);
    LIB_FUNCTION("RIa6GnWp+iU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strerror);
    LIB_FUNCTION("KZm8HUIX2Rw", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcscat);
    LIB_FUNCTION("Ezzq78ZgHPs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcschr);
    LIB_FUNCTION("pNtJdE3x49E", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcscmp);
    LIB_FUNCTION("FM5NPnLqBc8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcscpy);
    LIB_FUNCTION("E8wCoUEbfzk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcsncmp);
    LIB_FUNCTION("0nV21JjYCH8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcsncpy);
    LIB_FUNCTION("g3ShSirD50I", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcsrchr);
    LIB_FUNCTION("WDpobjImAb4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcsstr);
    LIB_FUNCTION("DAbZ-Vfu6lQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcstoull);
    LIB_FUNCTION("sOOMlZoy1pg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_wcsrtombs);
    LIB_FUNCTION("8hygs6D9KBY", "libSceLibcInternal", 1, "libSceLibcInternal", internal_mbsrtowcs);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcStrAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("kiZSXIWd9vg", "libc", 1, "libc", internal_strcpy);
    LIB_FUNCTION("Ls4tzzhimqQ", "libc", 1, "libc", internal_strcat);
    LIB_FUNCTION("j4ViWNHEgww", "libc", 1, "libc", internal_strlen);
    LIB_FUNCTION("WkkeywLJcgU", "libc", 1, "libc", internal_wcslen);
    LIB_FUNCTION("Ovb2dSJOAuE", "libc", 1, "libc", internal_strcmp);
    LIB_FUNCTION("6sJWiWSRuqk", "libc", 1, "libc", internal_strncpy);
    LIB_FUNCTION("ob5xAW4ln-0", "libc", 1, "libc", internal_strchr);
    LIB_FUNCTION("AV6ipCNa4Rw", "libc", 1, "libc", internal_strcasecmp);
    LIB_FUNCTION("q0F6yS-rCms", "libc", 1, "libc", internal_strcspn);
    LIB_FUNCTION("aesyjrHVWy4", "libc", 1, "libc", internal_strncmp);
    LIB_FUNCTION("kHg45qPC6f0", "libc", 1, "libc", internal_strncat);
    LIB_FUNCTION("9yDWMxEFdJU", "libc", 1, "libc", internal_strrchr);
    LIB_FUNCTION("-kU6bB4M-+k", "libc", 1, "libc", internal_strspn);
    LIB_FUNCTION("viiwFMaNamA", "libc", 1, "libc", internal_strstr);
    LIB_FUNCTION("oVkZ8W8-Q8A", "libc", 1, "libc", internal_strtok);
    LIB_FUNCTION("mXlxhmLNMPg", "libc", 1, "libc", internal_strtol);
    LIB_FUNCTION("RIa6GnWp+iU", "libc", 1, "libc", internal_strerror);
    LIB_FUNCTION("KZm8HUIX2Rw", "libc", 1, "libc", internal_wcscat);
    LIB_FUNCTION("Ezzq78ZgHPs", "libc", 1, "libc", internal_wcschr);
    LIB_FUNCTION("pNtJdE3x49E", "libc", 1, "libc", internal_wcscmp);
    LIB_FUNCTION("FM5NPnLqBc8", "libc", 1, "libc", internal_wcscpy);
    LIB_FUNCTION("E8wCoUEbfzk", "libc", 1, "libc", internal_wcsncmp);
    LIB_FUNCTION("0nV21JjYCH8", "libc", 1, "libc", internal_wcsncpy);
    LIB_FUNCTION("g3ShSirD50I", "libc", 1, "libc", internal_wcsrchr);
    LIB_FUNCTION("WDpobjImAb4", "libc", 1, "libc", internal_wcsstr);
    LIB_FUNCTION("DAbZ-Vfu6lQ", "libc", 1, "libc", internal_wcstoull);
    LIB_FUNCTION("sOOMlZoy1pg", "libc", 1, "libc", internal_wcsrtombs);
    LIB_FUNCTION("8hygs6D9KBY", "libc", 1, "libc", internal_mbsrtowcs);
}
#endif

} // namespace Libraries::LibcInternal
