// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

// FreeBSD usrstack (~0x7EFFF8000) sits inside SYSTEM_MANAGED. On Android that
// VA range already holds host thread stacks, so a guest pthread stack mapped
// there is not 1:1. FEX then writes the return address through the host
// pointer while the guest [RSP] load reads the shadow mapping.
constexpr VAddr kUsrStackTop = 0x7EFFF8000ULL;
constexpr VAddr kHostOverlapStackMin = 0x7E0000000ULL;
constexpr VAddr kHostOverlapStackMax = 0x800000000ULL;

[[nodiscard]] inline bool PthreadStackOverlapsHost(VAddr addr, size_t size) {
    if (addr == 0 || size == 0) {
        return false;
    }
    if (addr >= kHostOverlapStackMax) {
        return false;
    }
    const VAddr end = addr + size;
    if (end < addr) {
        return true;
    }
    return end > kHostOverlapStackMin;
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
inline constexpr bool kPlacePthreadStackInGuestSafeRange = true;
#else
inline constexpr bool kPlacePthreadStackInGuestSafeRange = false;
#endif

} // namespace Libraries::Kernel
