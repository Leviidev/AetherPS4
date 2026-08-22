// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/guest_cpu/guest_memory_validation_cache.h"
#include "core/memory_map_generation.h"

namespace {

using Core::GuestCpu::GuestMemoryValidationCache;
using Core::MemoryMapGeneration;

constexpr std::uint32_t Read = 1;
constexpr std::uint32_t Write = 2;

TEST(GuestMemoryValidationCache, ReusesOnlyMatchingOwnerGenerationRangeAndPermissions) {
    int owner{};
    int other_owner{};
    GuestMemoryValidationCache cache;

    cache.Store(&owner, 4, 0x1000, 0x3000, Read | Write);

    EXPECT_TRUE(cache.Contains(&owner, 4, 0x1800, 0x100, Read));
    EXPECT_TRUE(cache.Contains(&owner, 4, 0x1800, 0x100, Read | Write));
    EXPECT_FALSE(cache.Contains(&other_owner, 4, 0x1800, 0x100, Read));
    EXPECT_FALSE(cache.Contains(&owner, 6, 0x1800, 0x100, Read));
    EXPECT_FALSE(cache.Contains(&owner, 4, 0x0800, 0x100, Read));
    EXPECT_FALSE(cache.Contains(&owner, 4, 0x2f80, 0x100, Read));
}

TEST(GuestMemoryValidationCache, RejectsMutationAndMalformedRequests) {
    int owner{};
    GuestMemoryValidationCache cache;

    cache.Store(&owner, 4, 0x1000, 0x3000, Read);

    EXPECT_FALSE(cache.Contains(&owner, 5, 0x1800, 0x100, Read));
    EXPECT_FALSE(cache.Contains(&owner, 4, 0x1800, 0x100, Read | Write));
    EXPECT_FALSE(cache.Contains(&owner, 4, 0, 0x100, Read));
    EXPECT_FALSE(cache.Contains(&owner, 4, 0x1800, 0, Read));
    EXPECT_FALSE(cache.Contains(&owner, 4, UINTPTR_MAX - 0x10, 0x20, Read));
}

TEST(GuestMemoryValidationCache, RetainsAlternatingPointerRanges) {
    int owner{};
    GuestMemoryValidationCache cache;

    cache.Store(&owner, 4, 0x1000, 0x2000, Read | Write);
    cache.Store(&owner, 4, 0x8000, 0xa000, Read);

    EXPECT_TRUE(cache.Contains(&owner, 4, 0x1800, 0x100, Read | Write));
    EXPECT_TRUE(cache.Contains(&owner, 4, 0x9000, 0x100, Read));
}

TEST(MemoryMapGeneration, IsOddOnlyWhileWriterMutationIsActive) {
    MemoryMapGeneration generation;

    EXPECT_EQ(generation.Load(), 0U);
    {
        const auto mutation = generation.BeginMutation();
        EXPECT_EQ(generation.Load(), 1U);
    }
    EXPECT_EQ(generation.Load(), 2U);
}

TEST(MemoryMapGeneration, ExplicitCompletionIsIdempotentBeforeWriterUnlock) {
    MemoryMapGeneration generation;

    {
        auto mutation = generation.BeginMutation();
        mutation.Finish();
        mutation.Finish();
        EXPECT_EQ(generation.Load(), 2U);
    }
    EXPECT_EQ(generation.Load(), 2U);
}

} // namespace
