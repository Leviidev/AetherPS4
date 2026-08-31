// SPDX-License-Identifier: MIT

#include "fex_guest_cpu.h"

#include <cerrno>
#include <cstdlib>
#include <utility>

namespace Core {
namespace {

GuestExecutionStage ToGuestStage(AetherPS4::Fex::EngineStage stage) {
    switch (stage) {
    case AetherPS4::Fex::EngineStage::Request:
        return GuestExecutionStage::Request;
    case AetherPS4::Fex::EngineStage::Mapping:
    case AetherPS4::Fex::EngineStage::Invalidate:
        return GuestExecutionStage::Mapping;
    case AetherPS4::Fex::EngineStage::Thread:
        return GuestExecutionStage::Thread;
    case AetherPS4::Fex::EngineStage::Teardown:
        return GuestExecutionStage::Teardown;
    case AetherPS4::Fex::EngineStage::Config:
    case AetherPS4::Fex::EngineStage::Context:
    case AetherPS4::Fex::EngineStage::Execute:
    case AetherPS4::Fex::EngineStage::Bridge:
        return GuestExecutionStage::Execute;
    }
    return GuestExecutionStage::Execute;
}

GuestExecutionFailure ToGuestFailure(const AetherPS4::Fex::EngineFailure& failure) {
    return {ToGuestStage(failure.Stage), failure.Error == 0 ? EIO : failure.Error};
}

} // namespace

FexGuestCpuBackend::FexGuestCpuBackend(std::unique_ptr<AetherPS4::Fex::GuestEngine> engine_)
    : engine{std::move(engine_)} {}

FexGuestCpuBackend::~FexGuestCpuBackend() = default;

FexGuestCpuBackend::Thread::~Thread() {
    if (thread != nullptr) {
        owner.DestroyThreadOrAbort(*this);
    }
}

FexGuestCpuBackend::CreateResult FexGuestCpuBackend::Create(AetherPS4::Fex::GuestBridge& bridge) {
    auto result = AetherPS4::Fex::GuestEngine::Create(bridge);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::unique_ptr<FexGuestCpuBackend>{new FexGuestCpuBackend{std::move(std::get<std::unique_ptr<AetherPS4::Fex::GuestEngine>>(result))}};
}

FexGuestCpuBackend::ThreadResult FexGuestCpuBackend::CreateThread(const GuestExecutionRequest& request) {
    if (engine == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->CreateThread(request);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::unique_ptr<Thread>{new Thread{*this, std::get<AetherPS4::Fex::GuestEngine::Thread*>(result)}};
}

GuestExecutionResult FexGuestCpuBackend::Run(Thread& thread) {
    if (engine == nullptr || thread.thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->Run(*thread.thread);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::get<GuestExecutionState>(result);
}

GuestExecutionResult FexGuestCpuBackend::CallGuest(
    std::uintptr_t rip, std::span<const std::uint64_t> arguments) {
    if (engine == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->CallGuest(rip, arguments);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::get<GuestExecutionState>(result);
}

FexGuestCpuBackend::OperationResult FexGuestCpuBackend::Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size) {
    if (engine == nullptr || thread.thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->Invalidate(*thread.thread, begin, size);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return true;
}

FexGuestCpuBackend::OperationResult FexGuestCpuBackend::DestroyThread(std::unique_ptr<Thread>& thread) {
    if (engine == nullptr || thread == nullptr || thread->thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto* nativeThread = thread->thread;
    auto result = engine->DestroyThread(nativeThread);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    thread->thread = nullptr;
    thread.reset();
    return true;
}

void FexGuestCpuBackend::DestroyThreadOrAbort(Thread& thread) noexcept {
    if (engine == nullptr || thread.thread == nullptr) {
        std::abort();
    }
    auto* nativeThread = thread.thread;
    const auto result = engine->DestroyThread(nativeThread);
    if (std::holds_alternative<AetherPS4::Fex::EngineFailure>(result)) {
        std::abort();
    }
    thread.thread = nullptr;
}

GuestExecutionResult FexGuestCpuBackend::Run(const GuestExecutionRequest& request) {
    auto threadResult = CreateThread(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&threadResult)) {
        return *failure;
    }
    auto thread = std::move(std::get<std::unique_ptr<Thread>>(threadResult));
    auto runResult = Run(*thread);
    const auto teardownResult = DestroyThread(thread);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&runResult)) {
        return *failure;
    }
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&teardownResult)) {
        return *failure;
    }
    return std::get<GuestExecutionState>(runResult);
}

std::uintptr_t FexGuestCpuBackend::ReturnAddress() const {
    return engine == nullptr ? 0 : engine->ReturnAddress();
}

GuestExecutionRange FexGuestCpuBackend::ReturnRange() const {
    return engine == nullptr ? GuestExecutionRange{} : engine->ReturnRange();
}

GuestExecutionRange FexGuestCpuBackend::CallbackReturnRange() const {
    return engine == nullptr ? GuestExecutionRange{} : engine->CallbackReturnRange();
}

} // namespace Core
