// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/debug.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// Ensure every 4 MB tracking manager covering a registered GPU buffer exists.
    ///
    /// A buffer can be written exclusively by the GPU before any CPU upload occurs. The
    /// upload path used to be the only path that lazily created RegionManagers, so those
    /// write-only buffers had no modification state. A later CPU readback consequently
    /// skipped the GPU download and observed stale zero-filled guest memory. Register the
    /// tracking range at the same time as the buffer cache entry instead.
    void TrackRegion(VAddr cpu_addr, u64 size) {
        IteratePages<true>(cpu_addr, size,
                           [](RegionManager*, u64, size_t) { /* Creation is the operation. */ });
    }

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                std::scoped_lock lk{manager->lock};
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<true>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<true>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        IteratePages<true>(
            cpu_addr, size, [&on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload) {
        IteratePages<true>(query_cpu_range, query_size,
                           [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                               manager->lock.lock();
                               manager->template ForEachModifiedRange<Type::CPU, true>(
                                   manager->GetCpuAddr() + offset, size, func);
                               if (!is_written) {
                                   manager->lock.unlock();
                               }
                           });
        on_upload();
        if (!is_written) {
            return;
        }
        IteratePages<true>(query_cpu_range, query_size,
                            [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                                manager->lock.unlock();
                            });
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        // GPU command processing can discover a direct-memory buffer before the normal CPU
        // upload/registration path sees it (Journey's resource streamer does this). Never skip
        // modification tracking for that range: create its manager here so a later readback does
        // not expose stale zero-filled guest memory.
        IteratePages<true>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            if (page_index >= NUM_HIGH_PAGES) [[unlikely]] {
                LOG_CRITICAL(Render_Vulkan,
                             "Memory tracker address exceeds its {}-bit range: addr={:#x}, "
                             "size={:#x}",
                             MAX_CPU_PAGE_BITS, cpu_address, size);
                return false;
            }
            auto* manager{top_tier[page_index].load(std::memory_order_acquire)};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                manager = CreateRegion(page_index);
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else {
                LOG_CRITICAL(Render_Vulkan,
                             "BACHATA_NO_MANAGER: page_index={:#x} addr={:#x} -- no RegionManager "
                             "was ever created for this 4MB region, call silently skipped",
                             page_index, page_index << TRACKER_HIGHER_PAGE_BITS);
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    RegionManager* CreateRegion(std::size_t page_index) {
        std::scoped_lock lock{manager_pool_mutex};
        if (auto* existing = top_tier[page_index].load(std::memory_order_relaxed)) {
            return existing;
        }
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                manager_pool.emplace_back(tracker, 0);
                free_managers.push_back(&manager_pool.back());
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index].store(new_manager, std::memory_order_release);
        return new_manager;
    }

    PageManager* tracker;
    std::mutex manager_pool_mutex;
    std::deque<RegionManager> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<std::atomic<RegionManager*>, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
