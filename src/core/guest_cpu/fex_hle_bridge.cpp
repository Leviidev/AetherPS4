// SPDX-License-Identifier: MIT

#include "fex_hle_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>

namespace {
thread_local Core::GuestCpu::HleCallFrame* ActiveHleCallFrame{};

class ActiveHleCallScope final {
public:
    explicit ActiveHleCallScope(Core::GuestCpu::HleCallFrame& frame)
        : previous{ActiveHleCallFrame} {
        ActiveHleCallFrame = &frame;
    }

    ~ActiveHleCallScope() {
        ActiveHleCallFrame = previous;
    }

private:
    Core::GuestCpu::HleCallFrame* previous;
};
} // namespace

namespace Core::GuestCpu {

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
    // Was a global first-256-calls-total budget. Confirmed on-device (three separate crash
    // investigations -- Sonic Mania, Rocket League, Journey) that this always exhausts during
    // boot alone: every one of those logs shows tracing stopping around index 250-255 within
    // the first couple thousand log lines, meaning nothing from actual gameplay -- where the
    // crashes this trace exists to help diagnose all actually happened -- was ever traced.
    // Trace the first call of each *distinct* function instead: an atomic exchange per
    // HleCallAdapter (traced_once), so a hot-polling API that crosses this bridge millions of
    // times still only logs once (on its first call), while a function that's never called
    // during the first 256 total calls -- like whatever void(void*) HLE function corrupted the
    // host allocator during Journey's actual gameplay -- still gets its one, only log line
    // whenever it first runs, no matter how deep into the session that is.
    const bool trace = !adapter->traced_once.exchange(true, std::memory_order_relaxed);
    if (trace) {
        std::fprintf(stderr, "BACHATA_FEX_HLE_BEGIN operation=%llu name=%.*s rsp=%#llx\n",
                     static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.rsp));
    }
    frame.validate_range = ValidateRange;
    frame.validate_context = this;
    frame.publish_host_range = PublishHostRange;
    frame.revoke_host_range = RevokeHostRange;
    frame.host_range_context = this;
    // Diagnostic only: RBX/RBP/R12-R15 are callee-saved in x86-64 SysV and no HLE adapter
    // legitimately returns a value through anything but gpr[0] (RAX) -- see EncodeReturn/
    // InvokeVoid, hle_call_adapter.h. If any of them change across this call, either an
    // adapter is writing somewhere it shouldn't, or the underlying native call this bridges to
    // isn't preserving guest register state the way the boundary assumes. Chasing a
    // deterministic guest crash (write through a corrupted `this`, guest RBX=1) that survived
    // a completely unrelated allocator fix -- narrowing whether the corruption happens right
    // here, at the one guest<->host register boundary in this whole call path.
    constexpr std::array<int, 5> calleeSaved{3, 5, 12, 13, 14}; // RBX, RBP, R12, R13, R14 (X86Enums.h indices)
    const auto beforeCalleeSaved = [&] {
        std::array<u64, calleeSaved.size()> values{};
        for (std::size_t i = 0; i < calleeSaved.size(); ++i) {
            values[i] = frame.gpr[calleeSaved[i]];
        }
        return values;
    }();
    const ActiveHleCallScope active_frame{frame};
    const auto result = adapter->Invoke(frame);
    for (std::size_t i = 0; i < calleeSaved.size(); ++i) {
        if (frame.gpr[calleeSaved[i]] != beforeCalleeSaved[i]) {
            std::fprintf(stderr,
                         "BACHATA_HLE_CALLEESAVED_CLOBBER operation=%llu name=%.*s reg_index=%d "
                         "before=%#llx after=%#llx\n",
                         static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         calleeSaved[i], static_cast<unsigned long long>(beforeCalleeSaved[i]),
                         static_cast<unsigned long long>(frame.gpr[calleeSaved[i]]));
        }
    }
    if (const auto* failure = std::get_if<HleCallFailure>(&result)) {
        if (trace) {
            std::fprintf(stderr, "BACHATA_FEX_HLE_END operation=%llu name=%.*s error=%d\n",
                         static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         failure->error);
        }
        Report(*failure);
        return AetherPS4::Fex::EngineFailure{AetherPS4::Fex::EngineStage::Bridge, failure->error};
    }
    if (trace) {
        std::fprintf(stderr, "BACHATA_FEX_HLE_END operation=%llu name=%.*s rax=%#llx\n",
                     static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.gpr[0]));
    }
    return true;
}

std::optional<GuestExecutionRange> HleGuestBridge::QueryExecutableRange(
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

} // namespace Core::GuestCpu
