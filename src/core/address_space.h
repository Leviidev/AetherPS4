// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <boost/icl/separate_interval_set.hpp>
#include "common/arch.h"
#include "common/enum.h"
#include "common/types.h"

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * Represents the user virtual address space backed by a dmem memory block
 */
class AddressSpace {
public:
    explicit AddressSpace();
    ~AddressSpace();

    [[nodiscard]] u8* BackingBase() const noexcept {
        return backing_base;
    }

    [[nodiscard]] VAddr SystemManagedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_managed_base);
    }
    [[nodiscard]] const u8* SystemManagedVirtualBase() const noexcept {
        return system_managed_base;
    }
    [[nodiscard]] u64 SystemManagedVirtualSize() const noexcept {
        return system_managed_size;
    }

    [[nodiscard]] VAddr SystemReservedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_reserved_base);
    }
    [[nodiscard]] const u8* SystemReservedVirtualBase() const noexcept {
        return system_reserved_base;
    }
    [[nodiscard]] u64 SystemReservedVirtualSize() const noexcept {
        return system_reserved_size;
    }

    [[nodiscard]] VAddr UserVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(user_base);
    }
    [[nodiscard]] const u8* UserVirtualBase() const noexcept {
        return user_base;
    }
    [[nodiscard]] u64 UserVirtualSize() const noexcept {
        return user_size;
    }

    /**
     * @brief Maps memory to the specified virtual address.
     * @param virtual_addr The base address to place the mapping.
     *        If zero is provided an address in system managed area is picked.
     * @param size The size of the area to map.
     * @param phys_addr The offset of the backing file handle to map.
     *                  The same backing region may be aliased into different virtual regions.
     *                  If zero is provided the mapping is considered as private.
     * @return A pointer to the mapped memory.
     */
    void* Map(VAddr virtual_addr, u64 size, PAddr phys_addr = -1, bool exec = false);

    /// Memory maps a specified file descriptor.
    void* MapFile(VAddr virtual_addr, u64 size, u64 offset, u32 prot, uintptr_t fd);

    /// Unmaps specified virtual memory area.
    void Unmap(VAddr virtual_addr, u64 size);

    /// Protects requested region.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);

    // Returns an interval set containing all usable regions.
    boost::icl::interval_set<VAddr> GetUsableRegions();

    // Games request Fixed mappings (see MemoryManager::MapMemory) at addresses that assume the
    // SDK-standard fixed layout other platforms reserve verbatim (the SYSTEM_MANAGED_MIN /
    // SYSTEM_RESERVED_MIN / USER_MIN constants defined at the top of address_space.cpp). On iOS
    // those constants are never actually used as the real base -- the sandbox refuses MAP_FIXED
    // at them outright, so the constructor instead reserves same-sized regions wherever
    // mmap(nullptr, ...) happens to place them and packs system_reserved_base/user_base
    // relative to that arbitrary base (see the TARGET_OS_IPHONE branch's own comment). A
    // perfectly valid, real-hardware address computed against the fixed constants can therefore
    // fall entirely outside every region this build actually tracks even though the *offset*
    // into its logical region is fine -- confirmed on-device as the cause of a GTA V memory
    // request that has no covering VMA at all. Since each logical region and its real
    // counterpart are always the same size (both come from the same *Size constants), any
    // address that legitimately falls within a logical region's [MIN, MAX] range maps losslessly
    // onto the equivalent offset in that region's real, currently-reserved counterpart. Returns
    // addr unchanged if it already falls inside an actual reserved region (nothing to do), or if
    // it doesn't correspond to any known logical region either (callers must still validate the
    // result -- this can't invent a valid address out of a genuinely bogus one).
    [[nodiscard]] VAddr TranslateFixedMappingAddress(VAddr addr) const noexcept;

    // Attempts to back [addr, addr+size) with real memory at that EXACT address via a direct,
    // fixed mmap -- see TranslateFixedMappingAddress's own comment on why rebasing alone isn't
    // enough for every caller: some games recompute the SDK-standard address later for direct
    // JIT loads/stores rather than remembering whatever a Fixed mapping syscall actually
    // returned, so the memory has to genuinely exist where they compute it, not merely have the
    // syscall report success from somewhere else. Deliberately narrow (exactly this one
    // mapping's range, not the whole logical region) to keep the blast radius small. Returns
    // false (host state unchanged) on any failure -- EACCES/ENOMEM/an actual collision --
    // rather than throwing or asserting, since callers have a rebase-based fallback for exactly
    // that case.
    [[nodiscard]] bool TryReserveExactRegion(VAddr addr, u64 size) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
};

} // namespace Core
