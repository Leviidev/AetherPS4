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
 * Detile/tile scratch allocation.
 *
 * Default (Turnip / BACHATA_MALI_GPU_OPT unset): mainline create+Defer destroy.
 *
 * Mali optimizations (BACHATA_MALI_GPU_OPT=1): multi-slot persistent ring that
 * avoids VMA free/reuse freeflight on system-vortek. Free rules in staging_diag.h.
 * Dig logs only with BACHATA_STAGING_VERBOSE=1.
 */
class TileManager {
    static constexpr size_t NUM_BPPS = 5;
    static constexpr u32 kScratchInitialSlots = 8;
    static constexpr u32 kScratchMaxSlots = 16;

public:
    using ScratchBuffer = std::pair<vk::Buffer, VmaAllocation>;
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

    /// Mainline path: allocate scratch and defer destroy after GPU work.
    ScratchBuffer GetScratchBuffer(u32 size);

    /// Mali ring path: acquire persistent scratch slot (never destroy while in use).
    ScratchSlot& AcquireScratchSlot(u32 size);
    void RefreshScratchCompletions();
    bool CreateScratchSlot(u32 capacity);
    void ResizeScratchSlot(ScratchSlot& slot, u32 need);
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
