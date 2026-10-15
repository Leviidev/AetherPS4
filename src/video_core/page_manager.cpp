// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <boost/container/small_vector.hpp>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "common/adaptive_mutex.h"
#ifdef ENABLE_USERFAULTFD
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "common/error.h"
#endif
#else
#include <windows.h>
#include "common/spin_lock.h"
#endif

#ifdef __linux__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;
#ifdef ENABLE_USERFAULTFD
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
        uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        ASSERT_MSG(uffd != -1, "{}", Common::GetLastErrorMsg());

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        ASSERT(ret == 0 && api.api == UFFD_API);

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    void OnMap(VAddr address, size_t size) {
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed");
    }

    void OnUnmap(VAddr address, size_t size) {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed");
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? 0 : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            ASSERT_MSG(readret != -1 || errno == EAGAIN, "Unexpected result of uffd read");
            if (errno == EAGAIN) {
                continue;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            rasterizer->InvalidateMemory(addr, 1);
        }
    }

    std::jthread ufd_thread;
    int uffd;
#else
    inline static Impl* s_instance = nullptr;

    // Ring buffer of recent real Protect() calls -- see DumpRecentPageProtects' doc comment in
    // page_manager.h for why this exists. A dedicated mutex, separate from `locks` (the
    // per-region RangeLockGuard array): recording happens while a region lock may already be
    // held, and this is only ever read afterward from the crash path, so there's no reason to
    // risk any ordering interaction with the real tracking locks.
    struct ProtectRecord {
        u64 sequence{};
        u64 thread_id{};
        VAddr addr{};
        size_t size{};
        Core::MemoryPermission requested_perms{};
        Core::MemoryPermission effective_perms{};
    };
    static constexpr size_t kProtectRingSize = 4096;
    std::mutex protect_ring_mutex{};
    std::array<ProtectRecord, kProtectRingSize> protect_ring{};
    u64 protect_sequence{};

    void RecordProtect(VAddr addr, size_t size, Core::MemoryPermission requested,
                       Core::MemoryPermission effective) {
        std::scoped_lock lk{protect_ring_mutex};
        auto& record = protect_ring[protect_sequence % kProtectRingSize];
        record.sequence = protect_sequence++;
#ifdef _WIN32
        record.thread_id = static_cast<u64>(GetCurrentThreadId());
#else
        record.thread_id = reinterpret_cast<u64>(pthread_self());
#endif
        record.addr = addr;
        record.size = size;
        record.requested_perms = requested;
        record.effective_perms = effective;
    }

    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
        s_instance = this;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    ~Impl() {
        if (s_instance == this) {
            s_instance = nullptr;
        }
    }

    void OnMap(VAddr address, size_t size) {
        // No-op
    }

    void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        // `perms` here is already whatever the caller (release_pending() in UpdatePageWatchers/
        // UpdatePageWatchersForRegion) decided the real hardware permission should be -- on
        // Apple that's already been through the sibling-intersection widening, so this is the
        // actual value about to be applied, not a pre-widening request. See DumpRecentPageProtects.
        RecordProtect(address, size, perms, perms);
        impl.Protect(address, size, perms);
    }

#ifdef __APPLE__
    // A permission or translation fault, as opposed to an alignment fault, which FEX's own
    // handler deals with.
    static bool IsPermissionFault(void* context) {
#if defined(ARCH_ARM64)
        // Translation and access flag faults (DFSC 0b0001xx, 0b0010xx) count too: a mapping being
        // replaced is briefly absent, and an access landing in that window faults although the
        // page is fine again.
        const auto dfsc = static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr & 0x3c;
        return dfsc == 0x0c || dfsc == 0x04 || dfsc == 0x08;
#else
        return true;
#endif
    }

    // Polls the host protection of `addr` for up to ~200 ms until it allows the access.
    static bool WaitForHostAccess(VAddr addr, bool is_write) {
        const vm_prot_t needed = is_write ? VM_PROT_WRITE : VM_PROT_READ;
        for (int i = 0; i < 2000; ++i) {
            vm_address_t region = static_cast<vm_address_t>(addr);
            vm_size_t size = 0;
            vm_region_basic_info_data_64_t info {};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                             reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS ||
                region > addr) {
                return false;
            }
            if ((info.protection & needed) == needed) {
                return true;
            }
            usleep(100);
        }
        LOG_CRITICAL(Render, "Fault at {:#x} (write={}) still not allowed after waiting", addr, is_write);
        return false;
    }
#endif

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        bool is_write = Common::IsWriteError(context);
        size_t write_watcher_count = 0;
        size_t read_watcher_count = 0;
        // A write to an unwatched 4KB page still faults while a sibling on its host page is
        // watched; invalidating that sibling is what lifts the protection.
        bool sibling_handled = false;
        if (s_instance) {
            const size_t page = addr >> PM_PAGE_BITS;
            // On 16KB host page systems (Darwin ARM64), 4 4KB PS4 pages share 1 host page.
            constexpr size_t SUBPAGES_PER_HOST_PAGE = 16_KB / PM_PAGE_SIZE;
            const size_t host_page_start = (page / SUBPAGES_PER_HOST_PAGE) * SUBPAGES_PER_HOST_PAGE;
            for (size_t p = host_page_start;
                 p < host_page_start + SUBPAGES_PER_HOST_PAGE && p < NUM_ADDRESS_PAGES; ++p) {
                const auto state = s_instance->cached_pages[p];
                write_watcher_count += state.num_write_watchers;
                read_watcher_count += state.num_read_watchers;
                if (state.num_write_watchers > 0) {
                    is_write = true;
                    if (p != page) {
                        sibling_handled |=
                            rasterizer->InvalidateMemory(p << PM_PAGE_BITS, PM_PAGE_SIZE);
                    }
                }
            }
        }
        if (write_watcher_count == 0 && read_watcher_count == 0) {
#ifdef __APPLE__
            // Another thread can drop the last watcher of this host page between this access
            // faulting and the handler running, with its mprotect still to land: retry once the
            // page allows the access, rather than treating it as nothing of ours. A page that
            // stays protected with no watchers is a real desync and still falls through.
            if (IsPermissionFault(context) && rasterizer->IsMapped(addr, 1) &&
                WaitForHostAccess(addr, is_write)) {
                return true;
            }
#endif
            return false;
        }
        const bool handled = is_write ? rasterizer->InvalidateMemory(addr, 8) || sibling_handled
                                      : rasterizer->ReadMemory(addr, 8);
#ifdef __APPLE__
        // Two threads faulting on the same page: the first invalidation lifts the protection
        // and leaves nothing for the second to do. Retry it once the page allows the access.
        if (!handled && IsPermissionFault(context) && WaitForHostAccess(addr, is_write)) {
            return true;
        }
#endif
        return handled;
    }
#endif

    // Applies `perms`, the watcher state of every 4KB page in [addr, addr + size), to that range.
    void ProtectWatched(VAddr addr, u64 size, Core::MemoryPermission perms) {
#ifdef __APPLE__
        // AddressSpace::Protect (address_space.cpp) rounds every mprotect() call out to the full
        // enclosing HOST page on Apple platforms (16KB on Apple Silicon/iOS ARM64), not the PS4's
        // own 4KB page granularity `perms` was computed at. Only the first and last host page of
        // the range can hold sibling 4KB pages from outside it, so only those get the intersection
        // (most-restrictive) of every sub-page sharing them -- what GuestFaultSignalHandler's
        // SUBPAGES_PER_HOST_PAGE loop expects. The host pages between are wholly this range's and
        // take `perms` as they are: widening the whole range from one watched sibling at an edge
        // left megabytes read-only that the bookkeeping counted as unwatched, and the next write
        // there faulted with no handler to claim it.
        const u64 host_page_size = static_cast<u64>(sysconf(_SC_PAGESIZE));
        if (host_page_size > PM_PAGE_SIZE) {
            const auto host_page_perms = [&](u64 host_addr) {
                auto effective = perms;
                for (u64 a = host_addr; a < host_addr + host_page_size; a += PM_PAGE_SIZE) {
                    const size_t p = a >> PM_PAGE_BITS;
                    if (p < NUM_ADDRESS_PAGES) {
                        effective &= cached_pages[p].Perms();
                    }
                }
                return effective;
            };
            const u64 end = addr + size;
            const u64 head = addr & ~(host_page_size - 1);
            const u64 tail = (end - 1) & ~(host_page_size - 1);
            if (head == tail) {
                Protect(addr, size, host_page_perms(head));
                return;
            }
            Protect(addr, head + host_page_size - addr, host_page_perms(head));
            if (head + host_page_size < tail) {
                Protect(head + host_page_size, tail - head - host_page_size, perms);
            }
            Protect(tail, end - tail, host_page_perms(tail));
            return;
        }
#endif
        Protect(addr, size, perms);
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ProtectWatched(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if constexpr (!track && !is_read) {
                if (new_count != 0) {
                    LOG_TRACE(Render_Vulkan,
                                 "BACHATA_STILL_PROTECTED: addr={:#x} new_count={} -- another "
                                 "reference is still holding write-protection on this page",
                                 page << PM_PAGE_BITS, new_count);
                }
            }

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ProtectWatched(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if constexpr (!track && !is_read) {
                if (update && new_count != 0) {
                    LOG_TRACE(Render_Vulkan,
                                 "BACHATA_STILL_PROTECTED: addr={:#x} new_count={} -- another "
                                 "reference is still holding write-protection on this page",
                                 (base_page + page) << PM_PAGE_BITS, new_count);
                }
            }

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_)
    : impl{std::make_unique<Impl>(rasterizer_)} {}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

bool PageManager::DumpRecentPageProtects(VAddr fault_addr, char* out_buf,
                                         std::size_t out_buf_size) noexcept {
#ifndef ENABLE_USERFAULTFD
    if (out_buf == nullptr || out_buf_size == 0 || Impl::s_instance == nullptr) {
        return false;
    }
    auto* instance = Impl::s_instance;
    char* w = out_buf;
    char* const end = out_buf + out_buf_size;
    const auto put = [&](const char* fmt, auto... args) {
        if (w < end) {
            const int n = std::snprintf(w, static_cast<std::size_t>(end - w), fmt, args...);
            if (n > 0) w += std::min<std::ptrdiff_t>(n, end - w);
        }
    };
    // The watcher counts of the faulting host page's 4KB pages (Mprotect rounds every call out to
    // the host page, so any of them can be what protects it).
    constexpr VAddr kHostPage = 16_KB;
    const VAddr host_page = fault_addr & ~(kHostPage - 1);
    put("host page %#llx watchers(w/r):", static_cast<unsigned long long>(host_page));
    for (VAddr a = host_page; a < host_page + kHostPage; a += PM_PAGE_SIZE) {
        const auto& state = instance->cached_pages[a >> PM_PAGE_BITS];
        put(" %u/%u", static_cast<unsigned>(state.num_write_watchers),
            static_cast<unsigned>(state.num_read_watchers));
    }
    put(" | ");
    std::scoped_lock lk{instance->protect_ring_mutex};
    const u64 sequence = instance->protect_sequence;
    const auto count = std::min<u64>(sequence, Impl::kProtectRingSize);
    // The newest calls that reached this host page, then the newest calls overall.
    int shown = 0;
    for (u64 i = 0; i < count && shown < 16; ++i) {
        const auto& rec = instance->protect_ring[(sequence - 1 - i) % Impl::kProtectRingSize];
        const VAddr lo = rec.addr & ~(kHostPage - 1);
        const VAddr hi = (rec.addr + rec.size + kHostPage - 1) & ~(kHostPage - 1);
        if (host_page >= lo && host_page < hi) {
            put("[seq=%llu addr=%#llx size=%#llx perms=%#x thread=%#llx%s] ",
                static_cast<unsigned long long>(rec.sequence), static_cast<unsigned long long>(rec.addr),
                static_cast<unsigned long long>(rec.size), static_cast<unsigned>(rec.effective_perms),
                static_cast<unsigned long long>(rec.thread_id),
                fault_addr >= rec.addr && fault_addr < rec.addr + rec.size ? " OVERLAPS_FAULT" : " HOST_PAGE");
            ++shown;
        }
    }
    put("| newest of %llu: ", static_cast<unsigned long long>(sequence));
    for (u64 i = 0; i < count && i < 24; ++i) {
        const auto& rec = instance->protect_ring[(sequence - 1 - i) % Impl::kProtectRingSize];
        put("[seq=%llu addr=%#llx size=%#llx perms=%#x thread=%#llx] ",
            static_cast<unsigned long long>(rec.sequence), static_cast<unsigned long long>(rec.addr),
            static_cast<unsigned long long>(rec.size), static_cast<unsigned>(rec.effective_perms),
            static_cast<unsigned long long>(rec.thread_id));
    }
    if (w < end) {
        *w = '\0';
    } else {
        out_buf[out_buf_size - 1] = '\0';
    }
    return true;
#else
    static_cast<void>(fault_addr);
    static_cast<void>(out_buf);
    static_cast<void>(out_buf_size);
    return false;
#endif
}

} // namespace VideoCore
