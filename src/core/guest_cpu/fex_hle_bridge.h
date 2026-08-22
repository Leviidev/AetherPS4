// SPDX-License-Identifier: MIT
#pragma once

#include "core/fex/fex_guest_engine.h"
#include "hle_call_adapter.h"

#include <optional>
#include <shared_mutex>
#include <vector>

namespace AetherPS4::GuestCpu {

class HleGuestBridge final : public AetherPS4::Fex::GuestBridge {
public:
    using RangeValidator = bool (*)(void* context, std::uintptr_t address, std::size_t size,
                                    bool writable);
    using ExecutableRangeQuery = std::optional<Core::GuestExecutionRange> (*)(void* context,
                                                                        std::uintptr_t address);
    using FailureReporter = void (*)(void* context, const HleCallFailure& failure);

    HleGuestBridge(HleCallRegistry& registry_, RangeValidator validator_, void* validator_context_,
                   FailureReporter reporter_ = nullptr, void* reporter_context_ = nullptr,
                   ExecutableRangeQuery executable_query_ = nullptr,
                   void* executable_query_context_ = nullptr);

    AetherPS4::Fex::EngineResult<bool> Invoke(HleCallFrame& frame) override;
    std::optional<Core::GuestExecutionRange> QueryExecutableRange(std::uintptr_t address) override;

private:
    struct HostRange final {
        std::uintptr_t begin{};
        std::uintptr_t end{};
        bool writable{};
    };

    static bool ValidateRange(void* context, std::uintptr_t address, std::size_t size,
                              bool writable);
    static bool PublishHostRange(void* context, std::uintptr_t address, std::size_t size,
                                 bool writable);
    static bool RevokeHostRange(void* context, std::uintptr_t address);
    bool ValidatePublishedHostRange(std::uintptr_t address, std::size_t size,
                                    bool writable) const;
    void Report(const HleCallFailure& failure) const;

    HleCallRegistry& registry;
    RangeValidator validator;
    void* validator_context;
    FailureReporter reporter;
    void* reporter_context;
    ExecutableRangeQuery executable_query;
    void* executable_query_context;
    mutable std::shared_mutex host_range_mutex;
    std::vector<HostRange> host_ranges;
};

} // namespace AetherPS4::GuestCpu
