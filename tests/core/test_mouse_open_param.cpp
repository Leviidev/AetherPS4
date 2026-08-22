// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/mouse/mouse_open_param.h"

namespace {

using Libraries::Mouse::MouseOpenBehaviour;
using Libraries::Mouse::OrbisMouseOpenParam;
using Libraries::Mouse::ResolveMouseOpenParam;

TEST(MouseOpenParam, NullParamUsesNormalDefaults) {
    OrbisMouseOpenParam storage{};
    storage.flag = MouseOpenBehaviour::Merged;
    const auto* resolved = ResolveMouseOpenParam(nullptr, storage);
    ASSERT_EQ(resolved, &storage);
    EXPECT_EQ(resolved->flag, MouseOpenBehaviour::Normal);
}

TEST(MouseOpenParam, ExplicitParamIsUnchanged) {
    OrbisMouseOpenParam param{};
    param.flag = MouseOpenBehaviour::Merged;
    OrbisMouseOpenParam storage{};
    const auto* resolved = ResolveMouseOpenParam(&param, storage);
    EXPECT_EQ(resolved, &param);
    EXPECT_EQ(resolved->flag, MouseOpenBehaviour::Merged);
}

} // namespace
