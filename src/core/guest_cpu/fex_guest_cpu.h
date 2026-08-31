// SPDX-License-Identifier: MIT
#pragma once

#include "../fex/fex_guest_engine.h"
#include "guest_cpu.h"

#include <memory>
#include <span>
#include <variant>

namespace Core {

class FexGuestCpuBackend final : public GuestCpuBackend {
public:
    class Thread final {
    public:
        ~Thread();

    private:
        friend class FexGuestCpuBackend;

        Thread(FexGuestCpuBackend& owner_, AetherPS4::Fex::GuestEngine::Thread* thread_)
            : owner{owner_}, thread{thread_} {}

        FexGuestCpuBackend& owner;
        AetherPS4::Fex::GuestEngine::Thread* thread{};
    };

    using CreateResult = std::variant<std::unique_ptr<FexGuestCpuBackend>, GuestExecutionFailure>;
    using ThreadResult = std::variant<std::unique_ptr<Thread>, GuestExecutionFailure>;
    using OperationResult = std::variant<bool, GuestExecutionFailure>;

    static CreateResult Create(AetherPS4::Fex::GuestBridge& bridge);

    FexGuestCpuBackend(const FexGuestCpuBackend&) = delete;
    FexGuestCpuBackend& operator=(const FexGuestCpuBackend&) = delete;
    ~FexGuestCpuBackend();

    ThreadResult CreateThread(const GuestExecutionRequest& request);
    GuestExecutionResult Run(Thread& thread);
    GuestExecutionResult CallGuest(std::uintptr_t rip,
                                   std::span<const std::uint64_t> arguments);
    OperationResult Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size);
    OperationResult DestroyThread(std::unique_ptr<Thread>& thread);
    GuestExecutionResult Run(const GuestExecutionRequest& request) override;
    std::uintptr_t ReturnAddress() const;
    GuestExecutionRange ReturnRange() const;
    GuestExecutionRange CallbackReturnRange() const;

private:
    explicit FexGuestCpuBackend(std::unique_ptr<AetherPS4::Fex::GuestEngine> engine_);
    void DestroyThreadOrAbort(Thread& thread) noexcept;

    std::unique_ptr<AetherPS4::Fex::GuestEngine> engine;
};

} // namespace Core
