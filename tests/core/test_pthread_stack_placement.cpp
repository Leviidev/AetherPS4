// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/kernel/threads/pthread_stack_placement.h"

namespace {

using Libraries::Kernel::kHostOverlapStackMax;
using Libraries::Kernel::kHostOverlapStackMin;
using Libraries::Kernel::kPlacePthreadStackInGuestSafeRange;
using Libraries::Kernel::kUsrStackTop;
using Libraries::Kernel::PthreadStackOverlapsHost;

TEST(PthreadStackPlacement, UsrstackRegionOverlapsHost) {
    constexpr size_t stack_size = 1 * 1024 * 1024;
    const VAddr stack_base = kUsrStackTop - stack_size;
    EXPECT_TRUE(PthreadStackOverlapsHost(stack_base, stack_size));
}

TEST(PthreadStackPlacement, DefaultMappingBaseDoesNotOverlapHost) {
    constexpr VAddr default_mapping_base = 0x200000000ULL;
    EXPECT_FALSE(PthreadStackOverlapsHost(default_mapping_base, 1 * 1024 * 1024));
}

TEST(PthreadStackPlacement, RejectsEmptyAndOverflowingRanges) {
    EXPECT_FALSE(PthreadStackOverlapsHost(0, 4096));
    EXPECT_FALSE(PthreadStackOverlapsHost(kUsrStackTop, 0));
    EXPECT_TRUE(PthreadStackOverlapsHost(kHostOverlapStackMin, ~size_t{0}));
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
TEST(PthreadStackPlacement, FexMustUseGuestSafeRange) {
    constexpr VAddr default_mapping_base = 0x200000000ULL;
    EXPECT_TRUE(kPlacePthreadStackInGuestSafeRange);
    EXPECT_LT(kHostOverlapStackMax, default_mapping_base);
}
#endif

} // namespace
