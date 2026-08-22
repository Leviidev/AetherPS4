// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/controller_snapshot.h"

#include <gtest/gtest.h>

using Platform::Bachata::ParseControllerSnapshot;

namespace {
std::string Line(std::string_view slot) {
    return "BACHATA/1 INPUT " + std::string(slot) +
           "seq=1 buttons=0 lx=128 ly=128 rx=128 ry=128 l2=0 r2=0 touch=0 tx=0 ty=0\n";
}
} // namespace

TEST(BachataControllerSnapshot, ParsesFourSlotsAndLegacySlotZero) {
    for (int slot = 0; slot < 4; ++slot) {
        const auto parsed = ParseControllerSnapshot(Line("slot=" + std::to_string(slot) + " "));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->slot, slot);
    }
    const auto legacy = ParseControllerSnapshot(Line(""));
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->slot, 0);
}

TEST(BachataControllerSnapshot, RejectsInvalidSlots) {
    EXPECT_FALSE(ParseControllerSnapshot(Line("slot=4 ")));
    EXPECT_FALSE(ParseControllerSnapshot(Line("slot=-1 ")));
}
