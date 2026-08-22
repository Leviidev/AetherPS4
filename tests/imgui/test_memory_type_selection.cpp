// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "imgui/renderer/imgui_memory.h"

namespace {

TEST(ImGuiMemoryTypeSelection, PrefersHostCoherentMemoryAndSkipsFlush) {
    VkPhysicalDeviceMemoryProperties properties{};
    properties.memoryTypeCount = 2;
    properties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    properties.memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    const auto selection = ImGui::Vulkan::SelectMemoryType(
        properties, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0b11,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    ASSERT_TRUE(selection.IsValid());
    EXPECT_EQ(selection.index, 1U);
    EXPECT_FALSE(selection.NeedsMappedMemoryFlush());
}

TEST(ImGuiMemoryTypeSelection, FlushesWhenOnlyNonCoherentMemoryIsAvailable) {
    VkPhysicalDeviceMemoryProperties properties{};
    properties.memoryTypeCount = 1;
    properties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    const auto selection = ImGui::Vulkan::SelectMemoryType(
        properties, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0b1,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    ASSERT_TRUE(selection.IsValid());
    EXPECT_EQ(selection.index, 0U);
    EXPECT_TRUE(selection.NeedsMappedMemoryFlush());
}

} // namespace
