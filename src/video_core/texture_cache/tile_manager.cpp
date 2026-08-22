// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/staging_diag.h"

#include "video_core/host_shaders/tiling_comp.h"

#include <algorithm>
#include <chrono>
#include <limits>
// std::count_if used in AcquireScratchSlot claim()

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

namespace {
constexpr u32 AlignScratchCapacity(u32 size) {
    // Round up to 256 KiB to reduce thrash on near-FHD sizes; FHD RGBA = 0x7f8000.
    constexpr u32 kGrain = 256 * 1024;
    return static_cast<u32>(Common::AlignUp(static_cast<u64>(size), kGrain));
}
} // namespace

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_},
      uses_push_descriptors{instance.IsPushDescriptorSupported()},
      desc_heap{instance, scheduler.GetMasterSemaphore(), pool_sizes, 64} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateFlags layout_flags =
        uses_push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                              : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = layout_flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);

    LogStagingDiagConfigOnce();
    if (MaliGpuOptEnabled()) {
        scratch_slots.reserve(kScratchMaxSlots);
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_POOL_INIT initialSlots={} maxSlots={} path=detile_scratch_ring "
                        "strictScratch={} tickLag={}",
                        kScratchInitialSlots, kScratchMaxSlots,
                        StagingDiag().strict_scratch ? 1 : 0, StagingDiag().tick_lag);
        }
    }
}

TileManager::~TileManager() {
    for (auto& slot : scratch_slots) {
        if (slot.buffer) {
            // Best-effort: wait GPU before tear-down so host does not free in-flight memory.
            if (slot.state == ScratchState::GpuInFlight && slot.tick != 0) {
                scheduler.Wait(slot.tick);
            }
            vmaDestroyBuffer(instance.GetAllocator(), slot.buffer, slot.allocation);
            slot.buffer = VK_NULL_HANDLE;
            slot.allocation = VK_NULL_HANDLE;
        }
    }
    scratch_slots.clear();
}

TileManager::ScratchBuffer TileManager::GetScratchBuffer(u32 size) {
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    ASSERT(result == VK_SUCCESS);
    return {vk::Buffer{buffer}, allocation};
}

void TileManager::RefreshScratchCompletions() {
    // Slot.tick is the scheduler tick of the open cmdbuf when acquired (StreamBuffer-style).
    // That tick is only signaled on the next SubmitExecution(NextTick). Until then
    // CurrentTick() == slot.tick and the work has NOT been submitted — must not free.
    // Smoking-gun failure: IsFree(tick) alone returned true on the same unsubmitted tick
    // under Vortek, so every detile reused slot0 (same dstBuffer) → Mali CS_BUS_FAULT.
    //
    // Mode B (strict_scratch): never free here on optimistic IsFree — only Wait() path frees.
    // Mode E (tick_lag): require cpuTick >= tick + lag before free (diag only).
    const u64 cpu_tick = scheduler.CurrentTick();
    const auto& diag = StagingDiag();
    for (u32 i = 0; i < scratch_slots.size(); ++i) {
        auto& slot = scratch_slots[i];
        if (slot.state != ScratchState::GpuInFlight && slot.state != ScratchState::Recording &&
            slot.state != ScratchState::Submitted) {
            continue;
        }
        if (slot.tick == 0) {
            continue;
        }
        // Still on open cmdbuf (not submitted yet).
        if (slot.tick >= cpu_tick) {
            continue;
        }
        if (diag.strict_scratch) {
            // Lifecycle: FREE→RECORDING→SUBMITTED→HOST_COMPLETED(via Wait)→FREE.
            // IsFree alone is not host-GPU proof under Vortek timeline.
            continue;
        }
        // Lag only when explicitly set (Mali opt injects tick_lag=12). No silent floor.
        if (diag.tick_lag > 0 && cpu_tick < slot.tick + diag.tick_lag) {
            continue;
        }
        if (!scheduler.IsFree(slot.tick)) {
            continue;
        }
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_SLOT_COMPLETED slot={} generation={} tick={} cpuTick={} "
                        "capacity={:#x} mode=isfree lag={}",
                        i, slot.generation, slot.tick, cpu_tick, slot.capacity, diag.tick_lag);
        }
        slot.state = ScratchState::Free;
        slot.tick = 0;
    }
}

bool TileManager::CreateScratchSlot(u32 capacity) {
    if (scratch_slots.size() >= kScratchMaxSlots) {
        return false;
    }
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = capacity,
        .usage = usage,
    };
    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "STAGING_POOL_GROWN failed size={:#x} result={}", capacity,
               int(result));
    if (result != VK_SUCCESS || !buffer) {
        return false;
    }

    ScratchSlot slot{};
    slot.buffer = vk::Buffer{buffer};
    slot.allocation = allocation;
    slot.capacity = capacity;
    slot.generation = 0;
    slot.tick = 0;
    slot.state = ScratchState::Free;
    scratch_slots.push_back(slot);
    ++scratch_stats_grows;

    if (StagingVerbose()) {
        LOG_WARNING(Render_Vulkan,
                    "STAGING_POOL_GROWN slots={} capacity={:#x} totalBytes≈{:#x}",
                    scratch_slots.size(), capacity,
                    static_cast<u64>(scratch_slots.size()) * capacity);
    }
    return true;
}

void TileManager::MaybeLogScratchPoolStats() {
    const u64 tick = scheduler.CurrentTick();
    // ~every 300 ticks ≈ few seconds of frame work; keep low volume.
    if (scratch_stats_last_log_tick != 0 && tick < scratch_stats_last_log_tick + 300) {
        return;
    }
    scratch_stats_last_log_tick = tick;
    u32 free_n = 0;
    u32 busy_n = 0;
    u64 bytes = 0;
    for (const auto& s : scratch_slots) {
        bytes += s.capacity;
        if (s.state == ScratchState::Free) {
            ++free_n;
        } else {
            ++busy_n;
        }
    }
    if (!StagingVerbose()) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "STAGING_POOL_STATS slots={} free={} busy={} bytes={:#x} grows={} acquired={} "
                "waits={}",
                scratch_slots.size(), free_n, busy_n, bytes, scratch_stats_grows,
                scratch_stats_acquired, scratch_stats_waits);
}

void TileManager::ResizeScratchSlot(ScratchSlot& slot, u32 need) {
    if (slot.capacity >= need && slot.buffer) {
        return;
    }
    if (slot.buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), slot.buffer, slot.allocation);
        slot.buffer = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
    }
    const vk::BufferCreateInfo buffer_ci = {
        .size = need,
        .usage = vk::BufferUsageFlagBits::eUniformBuffer |
                 vk::BufferUsageFlagBits::eStorageBuffer |
                 vk::BufferUsageFlagBits::eTransferSrc |
                 vk::BufferUsageFlagBits::eTransferDst,
    };
    const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    const auto ci = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result =
        vmaCreateBuffer(instance.GetAllocator(), &ci, &alloc_info, &buffer, &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "STAGING_POOL resize failed need={:#x} result={}", need,
               int(result));
    slot.buffer = vk::Buffer{buffer};
    slot.allocation = allocation;
    slot.capacity = need;
    if (StagingVerbose()) {
        LOG_WARNING(Render_Vulkan, "STAGING_POOL_GROWN resize slot capacity={:#x}", need);
    }
}

TileManager::ScratchSlot& TileManager::AcquireScratchSlot(u32 size) {
    const u32 need = AlignScratchCapacity(size);
    RefreshScratchCompletions();

    // Prefer free slots already large enough; otherwise reuse any free slot (resize).
    // Bloodborne: pool filled with small (0x40000) slots then FHD need=0x800000 asserted
    // because exhaust path skipped undersized busy slots.
    auto try_find_free = [&](bool require_capacity) -> ScratchSlot* {
        if (scratch_slots.empty()) {
            return nullptr;
        }
        const u32 n = static_cast<u32>(scratch_slots.size());
        for (u32 k = 0; k < n; ++k) {
            const u32 i = (next_scratch_rr + k) % n;
            auto& slot = scratch_slots[i];
            if (slot.state != ScratchState::Free) {
                continue;
            }
            if (require_capacity && slot.capacity < need) {
                continue;
            }
            next_scratch_rr = (i + 1) % n;
            return &slot;
        }
        return nullptr;
    };

    auto claim = [&](ScratchSlot* free, bool reused) -> ScratchSlot& {
        if (free->capacity < need || !free->buffer) {
            ResizeScratchSlot(*free, need);
        }
        free->state = ScratchState::GpuInFlight;
        free->generation = next_scratch_generation++;
        free->tick = scheduler.CurrentTick();
        ++scratch_stats_acquired;
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_SLOT_ACQUIRED slot={} generation={} capacity={:#x} need={:#x} "
                        "tick={} reused={} freeLeft={} strictScratch={} tickLag={}",
                        static_cast<u32>(free - scratch_slots.data()), free->generation,
                        free->capacity, need, free->tick, reused ? 1 : 0,
                        static_cast<u32>(std::count_if(
                            scratch_slots.begin(), scratch_slots.end(),
                            [](const ScratchSlot& s) { return s.state == ScratchState::Free; })),
                        StagingDiag().strict_scratch ? 1 : 0, StagingDiag().tick_lag);
        }
        MaybeLogScratchPoolStats();
        return *free;
    };

    if (ScratchSlot* free = try_find_free(true)) {
        return claim(free, true);
    }
    if (ScratchSlot* free = try_find_free(false)) {
        return claim(free, true);
    }

    // Grow pool: first demand fills to initialSlots; later demand grows to maxSlots.
    const u32 target =
        scratch_slots.empty()
            ? kScratchInitialSlots
            : static_cast<u32>(std::min<size_t>(scratch_slots.size() + 1, kScratchMaxSlots));
    while (scratch_slots.size() < target && scratch_slots.size() < kScratchMaxSlots) {
        if (!CreateScratchSlot(need)) {
            break;
        }
    }

    RefreshScratchCompletions();
    if (ScratchSlot* free = try_find_free(true)) {
        return claim(free, false);
    }
    if (ScratchSlot* free = try_find_free(false)) {
        return claim(free, false);
    }

    if (scratch_slots.size() < kScratchMaxSlots && CreateScratchSlot(need)) {
        RefreshScratchCompletions();
        if (ScratchSlot* free = try_find_free(true)) {
            return claim(free, false);
        }
    }

    // All slots busy — wait oldest *submitted* slot of any capacity, free, resize, claim.
    // Never wait a tick still equal to CurrentTick (open cmdbuf).
    if (StagingVerbose()) {
        LOG_WARNING(Render_Vulkan,
                    "STAGING_POOL_EXHAUSTED slots={} need={:#x} — wait oldest exact tick",
                    scratch_slots.size(), need);
    }
    ++scratch_stats_waits;

    auto find_oldest_busy = [&](bool submitted_only) -> ScratchSlot* {
        const u64 cpu_tick = scheduler.CurrentTick();
        ScratchSlot* oldest = nullptr;
        for (auto& slot : scratch_slots) {
            if (slot.state != ScratchState::GpuInFlight && slot.state != ScratchState::Recording &&
                slot.state != ScratchState::Submitted) {
                continue;
            }
            if (submitted_only && (slot.tick == 0 || slot.tick >= cpu_tick)) {
                continue;
            }
            if (!oldest || slot.tick < oldest->tick) {
                oldest = &slot;
            }
        }
        return oldest;
    };

    ScratchSlot* oldest = find_oldest_busy(true);
    if (!oldest) {
        scheduler.Flush();
        RefreshScratchCompletions();
        if (ScratchSlot* free = try_find_free(true)) {
            return claim(free, true);
        }
        if (ScratchSlot* free = try_find_free(false)) {
            return claim(free, true);
        }
        oldest = find_oldest_busy(false);
    }
    if (!oldest) {
        // Last resort: allocate one more even past soft max is not allowed — resize any free
        // or force-wait every busy slot via queue idle so something becomes free.
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_POOL_EXHAUSTED slots={} need={:#x} — queue idle drain",
                        scratch_slots.size(), need);
        }
        scheduler.Flush();
        (void)instance.GetGraphicsQueue().waitIdle();
        for (auto& slot : scratch_slots) {
            slot.state = ScratchState::Free;
            slot.tick = 0;
        }
        if (ScratchSlot* free = try_find_free(false)) {
            return claim(free, true);
        }
        ASSERT_MSG(!scratch_slots.empty(), "STAGING_POOL empty after drain");
        return claim(&scratch_slots[0], true);
    }

    scheduler.Flush();
    const auto wait_begin = std::chrono::steady_clock::now();
    if (oldest->tick != 0) {
        scheduler.Wait(oldest->tick);
    }
    auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - wait_begin)
                       .count();
    if (wait_ms < 1) {
        const auto q_begin = std::chrono::steady_clock::now();
        (void)instance.GetGraphicsQueue().waitIdle();
        wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - q_begin)
                      .count();
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_SLOT_QUEUE_IDLE generation={} tick={} elapsedMs={}",
                        oldest->generation, oldest->tick, wait_ms);
        }
    }
    if (StagingVerbose()) {
        LOG_WARNING(Render_Vulkan,
                    "STAGING_SLOT_COMPLETED slot={} generation={} tick={} capacity={:#x} waited=1 "
                    "elapsedMs={} mode=exact_wait",
                    static_cast<u32>(oldest - scratch_slots.data()), oldest->generation,
                    oldest->tick, oldest->capacity, wait_ms);
    }
    oldest->state = ScratchState::Free;
    oldest->tick = 0;
    return claim(oldest, true);
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const u32 pl_id = u32(info.tile_mode) * NUM_BPPS + std::bit_width(info.num_bits) - 4;
    auto& tiling_pipelines = is_tiler ? tilers : detilers;
    if (auto pipeline = *tiling_pipelines[pl_id]; pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    std::vector<std::string> defines = {
        fmt::format("BITS_PER_PIXEL={}", info.num_bits),
        fmt::format("NUM_SAMPLES={}", info.num_samples),
        fmt::format("ARRAY_MODE={}", u32(info.array_mode)),
        fmt::format("MICRO_TILE_MODE={}", u32(micro_tile_mode)),
        fmt::format("MICRO_TILE_THICKNESS={}", AmdGpu::GetMicroTileThickness(info.array_mode)),
    };
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        const u32 num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        defines.emplace_back(
            fmt::format("PIPE_CONFIG={}", u32(AmdGpu::GetPipeConfig(info.tile_mode))));
        defines.emplace_back(fmt::format("BANK_WIDTH={}", AmdGpu::GetBankWidth(macro_tile_mode)));
        defines.emplace_back(fmt::format("BANK_HEIGHT={}", AmdGpu::GetBankHeight(macro_tile_mode)));
        defines.emplace_back(fmt::format("NUM_BANKS={}", num_banks));
        defines.emplace_back(fmt::format("NUM_BANK_BITS={}", std::bit_width(num_banks) - 1));
        defines.emplace_back(fmt::format(
            "TILE_SPLIT_BYTES={}", AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                              micro_tile_mode, info.num_bits)));
        defines.emplace_back(
            fmt::format("MACRO_TILE_ASPECT={}", AmdGpu::GetMacrotileAspect(macro_tile_mode)));
    }
    if (is_tiler) {
        defines.emplace_back(fmt::format("IS_TILER=1"));
    }

    const auto& module = Vulkan::Compile(HostShaders::TILING_COMP,
                                         vk::ShaderStageFlagBits::eCompute, device, defines);
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    for (const auto& def : defines) {
        LOG_INFO(Render_Vulkan, "#define {}", def);
    }
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    tiling_pipelines[pl_id] = std::move(pipeline);
    device.destroyShaderModule(module);
    return *tiling_pipelines[pl_id];
}

TileManager::Result TileManager::DetileImage(vk::Buffer in_buffer, u32 in_offset,
                                             const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    // Mali opt: persistent ring. Default: mainline create + deferred destroy.
    vk::Buffer out_buffer;
    if (MaliGpuOptEnabled()) {
        ScratchSlot& scratch = AcquireScratchSlot(info.guest_size);
        out_buffer = scratch.buffer;
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_SLOT_SUBMITTED generation={} tick={} op=detile size={:#x}",
                        scratch.generation, scratch.tick, info.guest_size);
        }
    } else {
        const auto [buf, allocation] = GetScratchBuffer(info.guest_size);
        out_buffer = buf;
        scheduler.DeferOperation([this, buf, allocation]() {
            vmaDestroyBuffer(instance.GetAllocator(), buf, allocation);
        });
    }

    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, false));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);
    } else {
        const auto desc_set = desc_heap.Commit(*desc_layout);
        std::array<vk::WriteDescriptorSet, 3> dst_writes = set_writes;
        for (auto& w : dst_writes) {
            w.dstSet = desc_set;
        }
        instance.GetDevice().updateDescriptorSets(dst_writes, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl_layout, 0, desc_set, {});
    }

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    if (StagingVerbose()) {
        LOG_WARNING(Render_Vulkan,
                    "GPU_ACCESS submission=client operation=detile shader={}_{} "
                    "srcBuffer={:#x} srcOffset={:#x} srcSize={:#x} "
                    "dstBuffer={:#x} dstOffset=0x0 dstSize={:#x} "
                    "width={} height={} pitch={} tilingMode={} guestSize={:#x} "
                    "isTiled={} arrayMode={} bpp={}",
                    magic_enum::enum_name(info.tile_mode), info.num_bits,
                    u64(VkBuffer(in_buffer)), in_offset, info.guest_size,
                    u64(VkBuffer(out_buffer)), info.guest_size, info.size.width, info.size.height,
                    info.pitch, u32(info.tile_mode), info.guest_size,
                    info.props.is_tiled ? 1 : 0, u32(info.array_mode), info.num_bits);
        if (!info.props.is_tiled || info.tile_mode == AmdGpu::TileMode::DisplayLinearAligned ||
            info.tile_mode == AmdGpu::TileMode::DisplayLinearGeneral) {
            LOG_WARNING(Render_Vulkan,
                        "GPU_RANGE_INVALID operation=detile reason=linear_or_untiled_into_detiler "
                        "tilingMode={} isTiled={}",
                        u32(info.tile_mode), info.props.is_tiled ? 1 : 0);
        }
    }
    cmdbuf.dispatch(dim_x, 1, 1);
    return {out_buffer, 0};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u32 out_offset, u32 copy_size) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer, out_offset, copy_size);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    vk::Buffer temp_buffer;
    if (MaliGpuOptEnabled()) {
        ScratchSlot& scratch = AcquireScratchSlot(info.guest_size);
        temp_buffer = scratch.buffer;
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "STAGING_SLOT_SUBMITTED generation={} tick={} op=tile size={:#x}",
                        scratch.generation, scratch.tick, info.guest_size);
        }
    } else {
        const auto [buf, allocation] = GetScratchBuffer(info.guest_size);
        temp_buffer = buf;
        scheduler.DeferOperation([this, buf, allocation]() {
            vmaDestroyBuffer(instance.GetAllocator(), buf, allocation);
        });
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    in_image.Download(buffer_copies, temp_buffer, 0, copy_size);

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, true));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);
    } else {
        const auto desc_set = desc_heap.Commit(*desc_layout);
        std::array<vk::WriteDescriptorSet, 3> dst_writes = set_writes;
        for (auto& w : dst_writes) {
            w.dstSet = desc_set;
        }
        instance.GetDevice().updateDescriptorSets(dst_writes, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl_layout, 0, desc_set, {});
    }

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    cmdbuf.dispatch(dim_x, 1, 1);
}

} // namespace VideoCore
