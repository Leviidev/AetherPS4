// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

    // Diagnostic-only: dumps the last 64 real Protect() calls (address, size, permission,
    // thread) into out_buf, most recent first -- entries whose range overlaps fault_addr are
    // marked. Added to chase a crash where a guest CPU thread reads through an ordinary heap
    // pointer and the VMM's own bookkeeping says the address is mapped, yet the host read still
    // faults. On Apple platforms a single Protect() call for one 4KB PS4 page gets rounded out
    // by AddressSpace::Protect to the full enclosing 16KB host page (see Impl::Protect's
    // Apple-specific sibling-permission-intersection logic) -- meaning a completely unrelated,
    // untracked 4KB neighbor sharing that host page can have its real hardware permission
    // collaterally changed by a GPU buffer's own tracking, with no update to its own watcher
    // bookkeeping. This says whether that's what actually happened here, and whether it points
    // at a live race or a permission that was never correctly restored. Returns false (out_buf
    // untouched) if no PageManager instance is currently active. Static (not tied to any one
    // PageManager instance) since the ring buffer it reads is itself process-wide, mirroring
    // Impl::s_instance/GuestFaultSignalHandler's own existing pattern.
    static bool DumpRecentPageProtects(VAddr fault_addr, char* out_buf,
                                       std::size_t out_buf_size) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
