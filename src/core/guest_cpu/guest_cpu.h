// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace Core {

enum class GuestExecutionStage {
    Request,
    Mapping,
    Thread,
    Execute,
    Teardown,
};

enum class GuestStopReason {
    Returned,
    Halted,
    Faulted,
};

struct GuestExecutionRange {
    std::uintptr_t Begin{};
    std::size_t Size{};
    bool Executable{};
    bool Writable{};
};

struct GuestExecutionRequest {
    std::uintptr_t Rip{};
    std::uintptr_t Rsp{};
    std::array<std::uint64_t, 16> Gpr{};
    std::uint64_t Rflags{};
    std::array<std::array<std::uint64_t, 2>, 16> Xmm{};
    std::uint64_t FsBase{};
    std::uint64_t GsBase{};
    std::vector<GuestExecutionRange> MappedRanges{};
};

struct GuestExecutionState {
    std::uintptr_t FirstRip{};
    std::uintptr_t Rip{};
    std::uintptr_t LastRip{};
    std::uintptr_t Rsp{};
    std::array<std::uint64_t, 16> Gpr{};
    std::uint64_t Rflags{};
    std::array<std::array<std::uint64_t, 2>, 16> Xmm{};
    GuestStopReason StopReason{};
};

struct GuestExecutionFailure {
    GuestExecutionStage Stage{};
    int Error{};
};

using GuestExecutionResult = std::variant<GuestExecutionState, GuestExecutionFailure>;

class GuestCpuBackend {
public:
    virtual ~GuestCpuBackend() = default;

    virtual GuestExecutionResult Run(const GuestExecutionRequest& request) = 0;
};

class NativeGuestCpuBackend final : public GuestCpuBackend {
public:
    using Executor = GuestExecutionResult (*)(const GuestExecutionRequest& request);

    explicit NativeGuestCpuBackend(Executor executor);

    GuestExecutionResult Run(const GuestExecutionRequest& request) override;

private:
    Executor executor{};
};

class UnavailableGuestCpuBackend final : public GuestCpuBackend {
public:
    GuestExecutionResult Run(const GuestExecutionRequest& request) override;
};

} // namespace Core
