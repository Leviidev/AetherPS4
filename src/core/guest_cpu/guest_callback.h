// SPDX-License-Identifier: MIT
#pragma once

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU

#include <array>
#include <cstdlib>
#include <span>
#include <string_view>
#include <type_traits>

#include <mach/mach.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/linker.h"

namespace AetherPS4::GuestCpu {

// Real (resident) memory footprint, matching what iOS's Jetsam out-of-memory killer actually
// tracks -- as opposed to virtual/reserved memory, which each guest thread's dedicated
// LookupCache uses ~272MB of without necessarily consuming real memory (Commit=false; see
// LookupCache.cpp). Logged at the point a guest callback fails to distinguish "genuine memory
// pressure" from any other cause of the same ENOMEM/stage-1 failure. Returns 0 on failure
// (never crashes/asserts -- this is diagnostic-only).
inline u64 BachataResidentMemoryBytes() noexcept {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return static_cast<u64>(info.phys_footprint);
}

template <typename T>
u64 EncodeGuestCallbackArgument(T value) {
    using Value = std::remove_cvref_t<T>;
    static_assert(std::is_integral_v<Value> || std::is_enum_v<Value> ||
                  std::is_pointer_v<Value> || std::is_same_v<Value, std::nullptr_t>);
    if constexpr (std::is_pointer_v<Value>) {
        return reinterpret_cast<u64>(value);
    } else if constexpr (std::is_same_v<Value, std::nullptr_t>) {
        return 0;
    } else {
        return static_cast<u64>(value);
    }
}

inline bool IsGuestFunctionAddress(const void* function) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    return function != nullptr && linker->FindByAddress(reinterpret_cast<VAddr>(function)) != nullptr;
}

inline u64 RunGuestFunctionOrAbort(const void* function, std::span<const u64> arguments,
                                   std::string_view label, VAddr stack_top = 0) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    LOG_INFO(Core_Linker, "BACHATA_GUEST_CALL: begin label={} function={:#x} stack_top={:#x}",
             label, reinterpret_cast<VAddr>(function), stack_top);
    const auto result = linker->RunGuestFunction(reinterpret_cast<VAddr>(function), arguments,
                                                  stack_top);
    LOG_INFO(Core_Linker, "BACHATA_GUEST_CALL: returned label={}", label);
    if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker,
                     "FEX guest callback {} failed at stage {}: {} (resident_mb={})", label,
                     static_cast<int>(failure->Stage), failure->Error,
                     BachataResidentMemoryBytes() / (1024 * 1024));
        std::abort();
    }
    return std::get<u64>(result);
}

template <typename... Args>
u64 RunGuestFunctionOrAbort(const void* function, std::string_view label, Args... args) {
    const std::array<u64, sizeof...(Args)> arguments{EncodeGuestCallbackArgument(args)...};
    return RunGuestFunctionOrAbort(function, arguments, label);
}

} // namespace AetherPS4::GuestCpu

#endif
