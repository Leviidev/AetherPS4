// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Core {

class MemoryMapGeneration final {
public:
    class Mutation final {
    public:
        explicit Mutation(MemoryMapGeneration& owner_) : owner{owner_} {
            owner.value.fetch_add(1, std::memory_order_acq_rel);
        }

        ~Mutation() {
            Finish();
        }

        void Finish() {
            if (!active) {
                return;
            }
            owner.value.fetch_add(1, std::memory_order_release);
            active = false;
        }

        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;

    private:
        MemoryMapGeneration& owner;
        bool active{true};
    };

    [[nodiscard]] Mutation BeginMutation() {
        return Mutation{*this};
    }

    [[nodiscard]] std::uint64_t Load() const {
        return value.load(std::memory_order_acquire);
    }

private:
    std::atomic_uint64_t value{};
};

} // namespace Core
