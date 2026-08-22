// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "core/libraries/videoout/buffer.h"

namespace Libraries::VideoOut {

// Tracks VideoOut flip-label generations so a presented buffer can retire on
// the next vblank without waiting for a replacement flip.
class FlipLabelTracker {
public:
    static constexpr u64 kInvalidGeneration = 0;

    std::optional<u64> RecordGpuLock(s32 index) {
        if (!Valid(index)) {
            return std::nullopt;
        }
        auto& generation = generations[static_cast<size_t>(index)];
        if (generation == UINT64_MAX) {
            generation = 1;
        } else {
            ++generation;
        }
        return generation;
    }

    u64 Generation(s32 index) const {
        if (!Valid(index)) {
            return kInvalidGeneration;
        }
        return generations[static_cast<size_t>(index)];
    }

    void ScheduleRetirement(s32 index, u64 generation, u64 due_vblank) {
        if (!Valid(index) || generation == kInvalidGeneration) {
            return;
        }
        pending = Pending{index, generation, due_vblank, true};
    }

    void CancelRetirementForIndex(s32 index) {
        if (pending.active && pending.index == index) {
            pending.active = false;
        }
    }

    std::optional<s32> ConsumeDueRetirement(u64 current_vblank) {
        if (!pending.active || current_vblank < pending.due_vblank) {
            return std::nullopt;
        }
        const Pending consumed = pending;
        pending.active = false;
        if (!Valid(consumed.index) ||
            generations[static_cast<size_t>(consumed.index)] != consumed.generation) {
            return std::nullopt;
        }
        return consumed.index;
    }

    void ResetBuffer(s32 index) {
        if (!Valid(index)) {
            return;
        }
        generations[static_cast<size_t>(index)] = kInvalidGeneration;
        CancelRetirementForIndex(index);
    }

    void ResetAll() {
        generations.fill(kInvalidGeneration);
        pending = {};
    }

private:
    static bool Valid(s32 index) {
        return index >= 0 && static_cast<size_t>(index) < MaxDisplayBuffers;
    }

    struct Pending {
        s32 index{-1};
        u64 generation{kInvalidGeneration};
        u64 due_vblank{};
        bool active{false};
    };

    std::array<u64, MaxDisplayBuffers> generations{};
    Pending pending{};
};

} // namespace Libraries::VideoOut
