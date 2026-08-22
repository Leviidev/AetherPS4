// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/videoout/flip_label_tracker.h"

namespace {

using Libraries::VideoOut::FlipLabelTracker;

TEST(FlipLabelTracker, RetiresPresentedGenerationOnNextVblank) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordGpuLock(0);
    ASSERT_TRUE(generation.has_value());
    tracker.ScheduleRetirement(0, *generation, 10);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(9).has_value());
    const auto retired = tracker.ConsumeDueRetirement(10);
    ASSERT_TRUE(retired.has_value());
    EXPECT_EQ(*retired, 0);
}

TEST(FlipLabelTracker, LaterLockInvalidatesStaleRetirement) {
    FlipLabelTracker tracker;
    const auto first = tracker.RecordGpuLock(0);
    ASSERT_TRUE(first.has_value());
    tracker.ScheduleRetirement(0, *first, 4);
    const auto second = tracker.RecordGpuLock(0);
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(*second, *first);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(4).has_value());
}

TEST(FlipLabelTracker, SupersedeCancelsDelayedRetirement) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordGpuLock(1);
    ASSERT_TRUE(generation.has_value());
    tracker.ScheduleRetirement(1, *generation, 8);
    tracker.CancelRetirementForIndex(1);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(8).has_value());
}

TEST(FlipLabelTracker, ResetBufferDropsGenerationAndPending) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordGpuLock(2);
    ASSERT_TRUE(generation.has_value());
    tracker.ScheduleRetirement(2, *generation, 3);
    tracker.ResetBuffer(2);
    EXPECT_EQ(tracker.Generation(2), FlipLabelTracker::kInvalidGeneration);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(3).has_value());
}

TEST(FlipLabelTracker, AlternatingBuffersRetireIndependently) {
    FlipLabelTracker tracker;
    const auto gen0 = tracker.RecordGpuLock(0);
    const auto gen1 = tracker.RecordGpuLock(1);
    ASSERT_TRUE(gen0.has_value());
    ASSERT_TRUE(gen1.has_value());
    tracker.ScheduleRetirement(0, *gen0, 5);
    tracker.CancelRetirementForIndex(0);
    tracker.ScheduleRetirement(1, *gen1, 6);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(5).has_value());
    const auto retired = tracker.ConsumeDueRetirement(6);
    ASSERT_TRUE(retired.has_value());
    EXPECT_EQ(*retired, 1);
}

TEST(FlipLabelTracker, InvalidGenerationNeverSchedules) {
    FlipLabelTracker tracker;
    tracker.ScheduleRetirement(0, FlipLabelTracker::kInvalidGeneration, 1);
    EXPECT_FALSE(tracker.ConsumeDueRetirement(1).has_value());
}

} // namespace
