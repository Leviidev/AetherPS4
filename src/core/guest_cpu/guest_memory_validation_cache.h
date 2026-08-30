// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace AetherPS4::GuestCpu {

class GuestMemoryValidationCache final {
public:
    bool Contains(const void* owner_, std::uint64_t generation_, std::uintptr_t address,
                  std::size_t size, std::uint32_t required_protection) const {
        if (owner_ == nullptr || address == 0 || size == 0 ||
            address > std::numeric_limits<std::uintptr_t>::max() - size ||
            (generation_ & 1) != 0) {
            return false;
        }
        const auto end_address = address + size;
        for (const auto& entry : entries) {
            if (entry.owner == owner_ && entry.generation == generation_ &&
                entry.begin <= address && end_address <= entry.end &&
                (entry.protection & required_protection) == required_protection) {
                return true;
            }
        }
        return false;
    }

    void Store(const void* owner_, std::uint64_t generation_, std::uintptr_t begin_,
               std::uintptr_t end_, std::uint32_t protection_) {
        entries[next_entry] = {owner_, generation_, begin_, end_, protection_};
        next_entry = (next_entry + 1) % entries.size();
    }

private:
    struct Entry final {
        const void* owner{};
        std::uint64_t generation{};
        std::uintptr_t begin{};
        std::uintptr_t end{};
        std::uint32_t protection{};
    };

    std::array<Entry, 4> entries{};
    std::size_t next_entry{};
};

} // namespace AetherPS4::GuestCpu
