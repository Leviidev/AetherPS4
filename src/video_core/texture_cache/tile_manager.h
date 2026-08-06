// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace VideoCore {

struct ImageInfo;
struct Image;
class StreamBuffer;

/**
 * Persistent detile/tile scratch ring (guest-side staging slots).
 *
 * Replaces create+destroy of FHD (0x7f8000) scratch buffers every frame —
 * that VMA free/reuse caused host SUBALLOC_REUSE_IN_FLIGHT / DEVICE_LOST on
 * Mali under system-vortek. Each slot owns a unique VkBuffer+memory for life
 * of TileManager.
 *
 * Free rules (see staging_diag.h A/B):
 *   A baseline: tick < CurrentTick && IsFree(tick)
 *   B strict_scratch: free only after scheduler.Wait (no optimistic IsFree)
 *   E tick_lag: also require cpuTick >= tick + lag
 *
 * Logs: STAGING_SLOT_ACQUIRED, STAGING_SLOT_SUBMITTED, STAGING_SLOT_COMPLETED,
 *       STAGING_POOL_GROWN, STAGING_POOL_EXHAUSTED, STAGING_POOL_STATS,
 *       STAGING_DIAG_CONFIG
 */
class TileManager {
    static constexpr size_t NUM_BPPS = 5;
    static constexpr u32 kScratchInitialSlots = 8;
    static constexpr u32 kScratchMaxSlots = 16;

public:
    using Result = std::pair<vk::Buffer, u32>;

    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   vk::Buffer out_buffer, u32 out_offset, u32 copy_size);

    Result DetileImage(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info);

private:
    enum class ScratchState : u8 {
        Free = 0,
        Recording,   ///< acquired, open cmdbuf (tick == CurrentTick)
        Submitted,   ///< tick advanced past slot.tick (submitted)
        GpuInFlight, ///< legacy alias: recording or submitted until free
    };

    struct ScratchSlot {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        u32 capacity{};
        u64 generation{};
        u64 tick{}; ///< scheduler tick of open cmdbuf when acquired; Wait this before Free
        ScratchState state{ScratchState::Free};
    };

    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler);

    /// Acquire persistent scratch slot (never destroy while in use).
    ScratchSlot& AcquireScratchSlot(u32 size);
    void RefreshScratchCompletions();
    bool CreateScratchSlot(u32 capacity);
    void MaybeLogScratchPoolStats();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    StreamBuffer& stream_buffer;
    bool uses_push_descriptors{};
    // Pool sizes must outlive desc_heap (DescriptorHeap stores a span to it).
    static constexpr std::array<vk::DescriptorPoolSize, 2> pool_sizes{{
        {vk::DescriptorType::eStorageBuffer, 64},
        {vk::DescriptorType::eUniformBuffer, 64},
    }};
    Vulkan::DescriptorHeap desc_heap;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> detilers{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> tilers{};

    std::vector<ScratchSlot> scratch_slots;
    u64 next_scratch_generation{1};
    u32 next_scratch_rr{0}; ///< round-robin cursor so free slots rotate
    u64 scratch_stats_acquired{};
    u64 scratch_stats_waits{};
    u64 scratch_stats_grows{};
    u64 scratch_stats_last_log_tick{};
};

} // namespace VideoCore
