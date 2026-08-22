// SPDX-License-Identifier: MIT

#include "fex_hle_bridge.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>

namespace {
constexpr uint32_t HleTraceLimit = 256;
std::atomic_uint32_t HleTraceCount{};

thread_local AetherPS4::GuestCpu::HleCallFrame* ActiveHleCallFrame{};

class ActiveHleCallScope final {
public:
    explicit ActiveHleCallScope(AetherPS4::GuestCpu::HleCallFrame& frame)
        : previous{ActiveHleCallFrame} {
        ActiveHleCallFrame = &frame;
    }

    ~ActiveHleCallScope() {
        ActiveHleCallFrame = previous;
    }

private:
    AetherPS4::GuestCpu::HleCallFrame* previous;
};
} // namespace

namespace AetherPS4::GuestCpu {

bool PublishHostRange(const void* pointer, std::size_t size, bool writable) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->publish_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->publish_host_range(
        ActiveHleCallFrame->host_range_context, reinterpret_cast<std::uintptr_t>(pointer), size,
        writable);
}

bool RevokeHostRange(const void* pointer) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->revoke_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->revoke_host_range(ActiveHleCallFrame->host_range_context,
                                                  reinterpret_cast<std::uintptr_t>(pointer));
}

HleGuestBridge::HleGuestBridge(HleCallRegistry& registry_, RangeValidator validator_,
                               void* validator_context_, FailureReporter reporter_,
                               void* reporter_context_, ExecutableRangeQuery executable_query_,
                               void* executable_query_context_)
    : registry{registry_}, validator{validator_}, validator_context{validator_context_},
      reporter{reporter_}, reporter_context{reporter_context_},
      executable_query{executable_query_}, executable_query_context{executable_query_context_} {}

AetherPS4::Fex::EngineResult<bool> HleGuestBridge::Invoke(HleCallFrame& frame) {
    const auto adapter = registry.Find(frame.operation);
    if (adapter == nullptr) {
        const HleCallFailure failure{ENOSYS, "unregistered HLE operation"};
        Report(failure);
        return AetherPS4::Fex::EngineFailure{AetherPS4::Fex::EngineStage::Bridge, failure.error};
    }
    // Tracing is intentionally bounded. Avoid a contended read-modify-write on every HLE call
    // after the trace window has filled; hot polling APIs can cross this bridge millions of times.
    auto trace_index = HleTraceCount.load(std::memory_order_relaxed);
    if (trace_index < HleTraceLimit) {
        trace_index = HleTraceCount.fetch_add(1, std::memory_order_relaxed);
    }
    const bool trace = trace_index < HleTraceLimit;
    if (trace) {
        std::fprintf(stderr,
                     "BACHATA_FEX_HLE_BEGIN index=%u operation=%llu name=%.*s rsp=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.rsp));
    }
    frame.validate_range = ValidateRange;
    frame.validate_context = this;
    frame.publish_host_range = PublishHostRange;
    frame.revoke_host_range = RevokeHostRange;
    frame.host_range_context = this;
    const ActiveHleCallScope active_frame{frame};
    const auto result = adapter->Invoke(frame);
    if (const auto* failure = std::get_if<HleCallFailure>(&result)) {
        if (trace) {
            std::fprintf(stderr,
                         "BACHATA_FEX_HLE_END index=%u operation=%llu name=%.*s error=%d\n",
                         trace_index, static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         failure->error);
        }
        Report(*failure);
        return AetherPS4::Fex::EngineFailure{AetherPS4::Fex::EngineStage::Bridge, failure->error};
    }
    if (trace) {
        std::fprintf(stderr,
                     "BACHATA_FEX_HLE_END index=%u operation=%llu name=%.*s rax=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.gpr[0]));
    }
    return true;
}

std::optional<Core::GuestExecutionRange> HleGuestBridge::QueryExecutableRange(
    std::uintptr_t address) {
    if (executable_query == nullptr) {
        return std::nullopt;
    }
    return executable_query(executable_query_context, address);
}

bool HleGuestBridge::ValidateRange(void* context, std::uintptr_t address, std::size_t size,
                                   bool writable) {
    if (context == nullptr) {
        return false;
    }
    const auto* bridge = static_cast<const HleGuestBridge*>(context);
    if (bridge->ValidatePublishedHostRange(address, size, writable)) {
        return true;
    }
    return bridge->validator != nullptr &&
           bridge->validator(bridge->validator_context, address, size, writable);
}

bool HleGuestBridge::PublishHostRange(void* context, std::uintptr_t address, std::size_t size,
                                      bool writable) {
    if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    const HostRange range{address, address + size, writable};
    std::unique_lock lock{bridge->host_range_mutex};
    const bool overlaps = std::ranges::any_of(bridge->host_ranges, [&](const HostRange& existing) {
        return range.begin < existing.end && existing.begin < range.end;
    });
    if (overlaps) {
        return false;
    }
    bridge->host_ranges.push_back(range);
    return true;
}

bool HleGuestBridge::RevokeHostRange(void* context, std::uintptr_t address) {
    if (context == nullptr || address == 0) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    std::unique_lock lock{bridge->host_range_mutex};
    const auto range = std::ranges::find(bridge->host_ranges, address, &HostRange::begin);
    if (range == bridge->host_ranges.end()) {
        return false;
    }
    bridge->host_ranges.erase(range);
    return true;
}

bool HleGuestBridge::ValidatePublishedHostRange(std::uintptr_t address, std::size_t size,
                                                bool writable) const {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    const auto end = address + size;
    std::shared_lock lock{host_range_mutex};
    return std::ranges::any_of(host_ranges, [&](const HostRange& range) {
        return range.begin <= address && end <= range.end && (!writable || range.writable);
    });
}

void HleGuestBridge::Report(const HleCallFailure& failure) const {
    if (reporter != nullptr) {
        reporter(reporter_context, failure);
    }
}

} // namespace AetherPS4::GuestCpu
