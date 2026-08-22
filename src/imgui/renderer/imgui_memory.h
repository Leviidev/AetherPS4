// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <limits>

#include <vulkan/vulkan_core.h>

namespace ImGui::Vulkan {

struct MemoryTypeSelection {
    uint32_t index = std::numeric_limits<uint32_t>::max();
    VkMemoryPropertyFlags property_flags{};

    [[nodiscard]] constexpr bool IsValid() const {
        return index != std::numeric_limits<uint32_t>::max();
    }

    [[nodiscard]] constexpr bool NeedsMappedMemoryFlush() const {
        return IsValid() && !(property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
};

[[nodiscard]] constexpr MemoryTypeSelection SelectMemoryType(
    const VkPhysicalDeviceMemoryProperties& properties, VkMemoryPropertyFlags required,
    uint32_t type_bits, VkMemoryPropertyFlags preferred = 0) {
    MemoryTypeSelection fallback{};

    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if (!(type_bits & (1U << index)) || (flags & required) != required) {
            continue;
        }

        const MemoryTypeSelection candidate{
            .index = index,
            .property_flags = flags,
        };
        if (preferred == 0 || (flags & preferred) == preferred) {
            return candidate;
        }
        if (!fallback.IsValid()) {
            fallback = candidate;
        }
    }

    return fallback;
}

} // namespace ImGui::Vulkan
