// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#include <gtest/gtest.h>

#include "common/path_util.h"

namespace {

TEST(PathUtil, CreatesAllMissingParentsForFreshSandbox) {
    namespace fs = std::filesystem;

    const fs::path test_root =
        fs::path(::testing::TempDir()) / "aetherps4-path-util" / "fresh-sandbox";
    const fs::path nested = test_root / "Library" / "Application Support" / "shadPS4";

    std::error_code ec;
    fs::remove_all(test_root, ec);

    ASSERT_TRUE(Common::FS::EnsureDirectoryTree(nested));
    EXPECT_TRUE(fs::is_directory(nested));

    // Repeated cold-launch setup must remain harmless.
    EXPECT_TRUE(Common::FS::EnsureDirectoryTree(nested));

    fs::remove_all(test_root, ec);
}

} // namespace
