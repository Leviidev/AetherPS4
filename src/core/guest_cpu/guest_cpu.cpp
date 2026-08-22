// SPDX-License-Identifier: MIT

#include "core/guest_cpu/guest_cpu.h"

#include <cerrno>

namespace Core {

NativeGuestCpuBackend::NativeGuestCpuBackend(Executor executor_) : executor{executor_} {}

GuestExecutionResult NativeGuestCpuBackend::Run(const GuestExecutionRequest& request) {
    if (executor == nullptr) {
        return GuestExecutionFailure{.Stage = GuestExecutionStage::Execute, .Error = ENOSYS};
    }
    return executor(request);
}

GuestExecutionResult UnavailableGuestCpuBackend::Run(const GuestExecutionRequest&) {
    return GuestExecutionFailure{.Stage = GuestExecutionStage::Execute, .Error = ENOTSUP};
}

} // namespace Core
