// SPDX-License-Identifier: MIT

#include "../../src/core/fex/fex_guest_engine.h"
#include "../../src/core/guest_cpu/fex_guest_cpu.h"
#include "../../src/core/guest_cpu/fex_hle_bridge.h"

#include <FEXCore/Core/X86Enums.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

constexpr uint64_t kHarnessBridgeOperation = 0xB4C4'F001ULL;
constexpr uint64_t kHarnessBridgeArgument = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kHarnessBridgeResult = 0x8877'6655'4433'2211ULL;
constexpr uint64_t kCallerMappedResult = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kNestedCallbackOperation = 0xB4C4'CA11ULL;
constexpr uint64_t kNestedCallbackInput = 37;
constexpr uint64_t kNestedCallbackResult = 42;
constexpr const char* kFexRevision = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec";

class Mapping final {
public:
  explicit Mapping(size_t size)
      : size{size}
      , address{mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)} {}

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  ~Mapping() {
    if (address != MAP_FAILED && munmap(address, size) != 0) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsValid() const { return address != MAP_FAILED; }
  [[nodiscard]] void* Get() const { return address; }
  [[nodiscard]] size_t Size() const { return size; }

  [[nodiscard]] bool MakeGuestExecutable() {
    __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
    return mprotect(address, size, PROT_READ) == 0;
  }

private:
  size_t size;
  void* address;
};

class HarnessBridge final : public AetherPS4::Fex::GuestBridge {
public:
  AetherPS4::Fex::EngineResult<bool> Invoke(AetherPS4::GuestCpu::HleCallFrame& frame) override {
    const auto operation = frame.operation;
    const auto argument = frame.gpr[FEXCore::X86State::REG_RDI];
    if (operation != kHarnessBridgeOperation || argument != kHarnessBridgeArgument) {
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, ENOSYS};
    }
    frame.gpr[FEXCore::X86State::REG_RAX] = kHarnessBridgeResult;
    return true;
  }
};

class NestedCallbackBridge final : public AetherPS4::Fex::GuestBridge {
public:
  AetherPS4::Fex::EngineResult<bool> Invoke(AetherPS4::GuestCpu::HleCallFrame& frame) override {
    if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
      std::fprintf(stderr,
                   "FEXCORE_GUEST_TRACE nested-bridge-enter op=%llx backend=%p callback=%p\n",
                   static_cast<unsigned long long>(frame.operation), static_cast<void*>(backend),
                   reinterpret_cast<void*>(callback));
      std::fflush(stderr);
    }
    if (frame.operation != kNestedCallbackOperation || backend == nullptr || callback == 0) {
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, ENOSYS};
    }
    const std::array<uint64_t, 1> arguments{kNestedCallbackInput};
    const auto result = backend->CallGuest(callback, arguments);
    if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&result)) {
      if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
        std::fprintf(stderr, "FEXCORE_GUEST_TRACE nested-call-fail stage=%d error=%d\n",
                     static_cast<int>(failure->Stage), failure->Error);
        std::fflush(stderr);
      }
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, failure->Error};
    }
    const auto& state = std::get<Core::GuestExecutionState>(result);
    if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
      std::fprintf(stderr, "FEXCORE_GUEST_TRACE nested-call-state stop=%d rax=%llu rip=%p\n",
                   static_cast<int>(state.StopReason),
                   static_cast<unsigned long long>(state.Gpr[FEXCore::X86State::REG_RAX]),
                   reinterpret_cast<void*>(state.Rip));
      std::fflush(stderr);
    }
    if (state.StopReason != Core::GuestStopReason::Returned) {
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, EPROTO};
    }
    frame.gpr[FEXCore::X86State::REG_RAX] = state.Gpr[FEXCore::X86State::REG_RAX];
    invoked = true;
    if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
      std::fprintf(stderr, "FEXCORE_GUEST_TRACE nested-bridge-invoked\n");
      std::fflush(stderr);
    }
    return true;
  }

  Core::FexGuestCpuBackend* backend{};
  std::uintptr_t callback{};
  bool invoked{};
};

uint64_t HleScalar(uint64_t left, uint64_t right) {
  return left + right;
}

uint64_t HlePointer(uint64_t* value, uint64_t increment) {
  if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
    std::fprintf(stderr, "FEXCORE_GUEST_TRACE hle-pointer-native value=%p increment=%llu\n",
                 static_cast<void*>(value), static_cast<unsigned long long>(increment));
    std::fflush(stderr);
  }
  *value += increment;
  return *value;
}

uint64_t HleFunctionPointer(uint64_t (*callback)(uint64_t)) {
  return reinterpret_cast<std::uintptr_t>(callback);
}

double HleVector(double left, double right) {
  return left * right;
}

uint64_t HleStack(uint64_t one, uint64_t two, uint64_t three, uint64_t four,
                  uint64_t five, uint64_t six, uint64_t seven) {
  if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
    std::fprintf(stderr,
                 "FEXCORE_GUEST_TRACE hle-stack-native args=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 static_cast<unsigned long long>(one), static_cast<unsigned long long>(two),
                 static_cast<unsigned long long>(three), static_cast<unsigned long long>(four),
                 static_cast<unsigned long long>(five), static_cast<unsigned long long>(six),
                 static_cast<unsigned long long>(seven));
    std::fflush(stderr);
  }
  return one + two + three + four + five + six + seven;
}

using RangeList = std::vector<Core::GuestExecutionRange>;

bool ValidateHarnessRange(void* context, std::uintptr_t address, std::size_t size, bool writable) {
  if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) return false;
  const auto& ranges = *static_cast<const RangeList*>(context);
  for (const auto& range : ranges) {
    const auto end = range.Begin + range.Size;
    if (address >= range.Begin && address + size <= end && (!writable || range.Writable)) {
      if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "FEXCORE_GUEST_TRACE range-ok address=%p size=%zu writable=%d begin=%p\n",
                     reinterpret_cast<void*>(address), size, writable,
                     reinterpret_cast<void*>(range.Begin));
        std::fflush(stderr);
      }
      return true;
    }
  }
  return false;
}

struct HleFailureCapture final {
  int error{};
  std::string name;
};

void CaptureHleFailure(void* context, const AetherPS4::GuestCpu::HleCallFailure& failure) {
  auto& capture = *static_cast<HleFailureCapture*>(context);
  capture.error = failure.error;
  capture.name = failure.name;
}

void Trace(const char* stage) {
  if (std::getenv("BACHATA_FEX_TRACE") == nullptr) return;
  std::fprintf(stderr, "FEXCORE_GUEST_TRACE %s\n", stage);
  std::fflush(stderr);
}

uint64_t DoubleBits(double value) {
  uint64_t bits{};
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

int Fail(const AetherPS4::Fex::EngineFailure& failure) {
  std::fprintf(stderr, "FEXCORE_GUEST_ENGINE_FAIL stage=%d error=%d\n", static_cast<int>(failure.Stage), failure.Error);
  return 1;
}

int Fail(const Core::GuestExecutionFailure& failure) {
  std::fprintf(stderr, "FEXCORE_GUEST_CPU_FAIL stage=%d error=%d\n", static_cast<int>(failure.Stage), failure.Error);
  return 1;
}

} // namespace

int main() {
  Trace("start");
  HarnessBridge bridge;
  auto engineResult = AetherPS4::Fex::GuestEngine::Create(bridge);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&engineResult)) return Fail(*failure);
  auto engine = std::move(std::get<std::unique_ptr<AetherPS4::Fex::GuestEngine>>(engineResult));

  auto runResult = engine->RunControlledHarness();
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&runResult)) return Fail(*failure);
  const auto result = std::get<AetherPS4::Fex::GuestRunResult>(runResult);
  if (!result.Gpr || !result.Rflags || !result.Xmm || !result.Bridge || !result.Threads ||
      !result.Tls || !result.Unaligned || !result.Invalidation) {
    return Fail({AetherPS4::Fex::EngineStage::Execute, EPROTO});
  }
  Trace("controlled-engine-ok");

  auto shutdownResult = engine->Shutdown();
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&shutdownResult)) return Fail(*failure);

  // pageSize is used consistently below for all Mapping sizing -- nothing here actually
  // requires the literal value 4096, so accept whatever the real host page size is (e.g.
  // 16384 on Apple Silicon) rather than assuming Linux/x86-64's 4096.
  const auto pageSize = sysconf(_SC_PAGESIZE);
  Mapping codePage{static_cast<size_t>(pageSize)};
  Mapping stackPage{static_cast<size_t>(pageSize)};
  if (!codePage.IsValid() || !stackPage.IsValid()) return 1;
  std::array<uint8_t, 11> code {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xf4}; // mov rax, imm64; hlt
  std::memcpy(code.data() + 2, &kCallerMappedResult, sizeof(kCallerMappedResult));
  std::memcpy(codePage.Get(), code.data(), code.size());
  if (!codePage.MakeGuestExecutable()) return 1;

  auto backendResult = Core::FexGuestCpuBackend::Create(bridge);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&backendResult)) return Fail(*failure);
  auto backend = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(backendResult));
  Core::GuestExecutionRequest request;
  request.Rip = reinterpret_cast<std::uintptr_t>(codePage.Get());
  request.Rsp = reinterpret_cast<std::uintptr_t>(stackPage.Get()) + pageSize - 16;
  request.Rflags = 1U << 1;
  request.MappedRanges = {
      {reinterpret_cast<std::uintptr_t>(codePage.Get()), static_cast<size_t>(pageSize), true, false},
      {reinterpret_cast<std::uintptr_t>(stackPage.Get()), static_cast<size_t>(pageSize), false, true},
  };
  auto threadResult = backend->CreateThread(request);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&threadResult)) return Fail(*failure);
  auto thread = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend::Thread>>(threadResult));
  auto callerRunResult = backend->Run(*thread);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&callerRunResult)) return Fail(*failure);
  const auto callerState = std::get<Core::GuestExecutionState>(callerRunResult);
  if (callerState.Gpr[static_cast<size_t>(FEXCore::X86State::REG_RAX)] != kCallerMappedResult ||
      callerState.Rsp != request.Rsp || callerState.StopReason != Core::GuestStopReason::Halted) {
    return Fail({Core::GuestExecutionStage::Execute, EPROTO});
  }
  auto invalidateResult = backend->Invalidate(*thread, request.Rip, static_cast<size_t>(pageSize));
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&invalidateResult)) return Fail(*failure);
  auto destroyResult = backend->DestroyThread(thread);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&destroyResult)) return Fail(*failure);

  Core::GuestExecutionRequest overlappingRequest = request;
  overlappingRequest.MappedRanges.insert(overlappingRequest.MappedRanges.begin() + 1,
                                         {request.Rip, static_cast<size_t>(pageSize), false, true});
  auto overlapResult = backend->CreateThread(overlappingRequest);
  const auto overlapRejected = std::get_if<Core::GuestExecutionFailure>(&overlapResult) != nullptr &&
                               std::get<Core::GuestExecutionFailure>(overlapResult).Stage == Core::GuestExecutionStage::Mapping &&
                               std::get<Core::GuestExecutionFailure>(overlapResult).Error == EACCES;
  if (!overlapRejected) return Fail({Core::GuestExecutionStage::Mapping, EPROTO});

  const auto runOwnerThread = [&backend, &codePage, pageSize]() {
    Mapping ownerStack {static_cast<size_t>(pageSize)};
    if (!ownerStack.IsValid()) return false;
    Core::GuestExecutionRequest ownerRequest;
    ownerRequest.Rip = reinterpret_cast<std::uintptr_t>(codePage.Get());
    ownerRequest.Rsp = reinterpret_cast<std::uintptr_t>(ownerStack.Get()) + pageSize - 16;
    ownerRequest.Rflags = 1U << 1;
    ownerRequest.MappedRanges = {
        {reinterpret_cast<std::uintptr_t>(codePage.Get()), static_cast<size_t>(pageSize), true, false},
        {reinterpret_cast<std::uintptr_t>(ownerStack.Get()), static_cast<size_t>(pageSize), false, true},
    };
    auto ownerThreadResult = backend->CreateThread(ownerRequest);
    if (std::holds_alternative<Core::GuestExecutionFailure>(ownerThreadResult)) return false;
    auto ownerThread = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend::Thread>>(ownerThreadResult));
    auto ownerRunResult = backend->Run(*ownerThread);
    if (std::holds_alternative<Core::GuestExecutionFailure>(ownerRunResult)) return false;
    const auto ownerState = std::get<Core::GuestExecutionState>(ownerRunResult);
    if (ownerState.Gpr[static_cast<size_t>(FEXCore::X86State::REG_RAX)] != kCallerMappedResult ||
        ownerState.FirstRip != ownerRequest.Rip || ownerState.LastRip != ownerState.Rip) {
      return false;
    }
    auto ownerDestroy = backend->DestroyThread(ownerThread);
    return std::holds_alternative<bool>(ownerDestroy);
  };
  bool firstOwnerThread {};
  bool secondOwnerThread {};
  std::thread firstHostThread {[&] { firstOwnerThread = runOwnerThread(); }};
  std::thread secondHostThread {[&] { secondOwnerThread = runOwnerThread(); }};
  firstHostThread.join();
  secondHostThread.join();
  if (!firstOwnerThread || !secondOwnerThread) return Fail({Core::GuestExecutionStage::Thread, EPROTO});
  Trace("guest-cpu-ok");

  Mapping nestedCode{static_cast<size_t>(pageSize)};
  Mapping nestedStack{static_cast<size_t>(pageSize)};
  if (!nestedCode.IsValid() || !nestedStack.IsValid()) {
    return Fail({Core::GuestExecutionStage::Mapping, ENOMEM});
  }
  std::array<uint8_t, 21> nestedBytes{
      0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x0f, 0x05, 0xf4, 0x90, 0x90, 0x90,
      0x48, 0x8d, 0x47, 0x05, 0xc3,
  };
  std::memcpy(nestedBytes.data() + 2, &kNestedCallbackOperation,
              sizeof(kNestedCallbackOperation));
  std::memcpy(nestedCode.Get(), nestedBytes.data(), nestedBytes.size());
  if (!nestedCode.MakeGuestExecutable()) {
    return Fail({Core::GuestExecutionStage::Mapping, errno});
  }

  NestedCallbackBridge nestedBridge;
  auto nestedBackendResult = Core::FexGuestCpuBackend::Create(nestedBridge);
  if (const auto* failure =
          std::get_if<Core::GuestExecutionFailure>(&nestedBackendResult)) {
    return Fail(*failure);
  }
  auto nestedBackend =
      std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(nestedBackendResult));
  nestedBridge.backend = nestedBackend.get();
  nestedBridge.callback = reinterpret_cast<std::uintptr_t>(nestedCode.Get()) + 16;

  Core::GuestExecutionRequest nestedRequest;
  nestedRequest.Rip = reinterpret_cast<std::uintptr_t>(nestedCode.Get());
  nestedRequest.Rsp = reinterpret_cast<std::uintptr_t>(nestedStack.Get()) + pageSize - 16;
  nestedRequest.Rflags = 1U << 1;
  nestedRequest.MappedRanges = {
      {reinterpret_cast<std::uintptr_t>(nestedCode.Get()), static_cast<size_t>(pageSize), true,
       false},
      {reinterpret_cast<std::uintptr_t>(nestedStack.Get()), static_cast<size_t>(pageSize), false,
       true},
      nestedBackend->ReturnRange(),
      nestedBackend->CallbackReturnRange(),
  };
  const auto nestedResult = nestedBackend->Run(nestedRequest);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&nestedResult)) {
    return Fail(*failure);
  }
  const auto& nestedState = std::get<Core::GuestExecutionState>(nestedResult);
  if (std::getenv("BACHATA_FEX_TRACE") != nullptr) {
    std::fprintf(stderr, "FEXCORE_GUEST_TRACE nested-outer-state invoked=%d rax=%llu\n",
                 nestedBridge.invoked,
                 static_cast<unsigned long long>(
                     nestedState.Gpr[FEXCore::X86State::REG_RAX]));
    std::fflush(stderr);
  }
  if (!nestedBridge.invoked ||
      nestedState.Gpr[FEXCore::X86State::REG_RAX] != kNestedCallbackResult) {
    return Fail({Core::GuestExecutionStage::Execute, EPROTO});
  }
  Trace("nested-callback-ok");

  AetherPS4::GuestCpu::HleCallRegistry hleRegistry;
  const auto scalarAdapter = hleRegistry.Register(AetherPS4::GuestCpu::MakeHleCallAdapter(HleScalar), "harness.scalar");
  const auto pointerAdapter = hleRegistry.Register(AetherPS4::GuestCpu::MakeHleCallAdapter(HlePointer), "harness.pointer");
  const auto functionPointerAdapter = hleRegistry.Register(
      AetherPS4::GuestCpu::MakeHleCallAdapter(HleFunctionPointer), "harness.function_pointer");
  const auto vectorAdapter = hleRegistry.Register(AetherPS4::GuestCpu::MakeHleCallAdapter(HleVector), "harness.vector");
  const auto stackAdapter = hleRegistry.Register(AetherPS4::GuestCpu::MakeHleCallAdapter(HleStack), "harness.stack");
  if (!scalarAdapter || !pointerAdapter || !functionPointerAdapter || !vectorAdapter ||
      !stackAdapter) {
    return Fail({Core::GuestExecutionStage::Execute, EPROTO});
  }

  AetherPS4::GuestCpu::HleVeneerAllocator veneerAllocator;
  const auto scalarVeneerResult = veneerAllocator.Allocate(*scalarAdapter);
  const auto pointerVeneerResult = veneerAllocator.Allocate(*pointerAdapter);
  const auto functionPointerVeneerResult = veneerAllocator.Allocate(*functionPointerAdapter);
  const auto vectorVeneerResult = veneerAllocator.Allocate(*vectorAdapter);
  const auto stackVeneerResult = veneerAllocator.Allocate(*stackAdapter);
  if (std::holds_alternative<AetherPS4::GuestCpu::HleVeneerFailure>(scalarVeneerResult) ||
      std::holds_alternative<AetherPS4::GuestCpu::HleVeneerFailure>(pointerVeneerResult) ||
      std::holds_alternative<AetherPS4::GuestCpu::HleVeneerFailure>(functionPointerVeneerResult) ||
      std::holds_alternative<AetherPS4::GuestCpu::HleVeneerFailure>(vectorVeneerResult) ||
      std::holds_alternative<AetherPS4::GuestCpu::HleVeneerFailure>(stackVeneerResult)) {
    return Fail({Core::GuestExecutionStage::Mapping, EIO});
  }
  const auto scalarVeneer = std::get<uint64_t>(scalarVeneerResult);
  const auto pointerVeneer = std::get<uint64_t>(pointerVeneerResult);
  const auto functionPointerVeneer = std::get<uint64_t>(functionPointerVeneerResult);
  const auto vectorVeneer = std::get<uint64_t>(vectorVeneerResult);
  const auto stackVeneer = std::get<uint64_t>(stackVeneerResult);
  const auto cachedScalar = veneerAllocator.Allocate(*scalarAdapter);
  auto veneerRanges = veneerAllocator.GetExecutableRanges();
  const bool mappingOk = std::holds_alternative<uint64_t>(cachedScalar) &&
                         std::get<uint64_t>(cachedScalar) == scalarVeneer && veneerRanges.size() == 5;
  if (!mappingOk) return Fail({Core::GuestExecutionStage::Mapping, EPROTO});

  Mapping hleReturnPage{static_cast<size_t>(pageSize)};
  if (!hleReturnPage.IsValid()) return Fail({Core::GuestExecutionStage::Mapping, ENOMEM});
  *static_cast<uint8_t*>(hleReturnPage.Get()) = 0xf4;
  if (!hleReturnPage.MakeGuestExecutable()) {
    return Fail({Core::GuestExecutionStage::Mapping, errno});
  }

  RangeList activeRanges;
  HleFailureCapture hleFailure;
  AetherPS4::GuestCpu::HleGuestBridge hleBridge{hleRegistry, ValidateHarnessRange, &activeRanges,
                                           CaptureHleFailure, &hleFailure};
  auto hleBackendResult = Core::FexGuestCpuBackend::Create(hleBridge);
  if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&hleBackendResult)) return Fail(*failure);
  auto hleBackend = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(hleBackendResult));
  Trace("hle-backend-ok");

  const auto runHle = [&](uint64_t veneer, const auto& prepare, const auto& verify,
                          std::span<const Core::GuestExecutionRange> extraRanges = {}) {
    Mapping callStack{static_cast<size_t>(pageSize)};
    if (!callStack.IsValid()) return false;
    const auto rsp = reinterpret_cast<std::uintptr_t>(callStack.Get()) + pageSize - 32;
    const auto returnRip = reinterpret_cast<std::uintptr_t>(hleReturnPage.Get());
    std::memcpy(reinterpret_cast<void*>(rsp), &returnRip, sizeof(returnRip));

    Core::GuestExecutionRequest hleRequest;
    hleRequest.Rip = veneer;
    hleRequest.Rsp = rsp;
    hleRequest.Rflags = 1U << 1;
    hleRequest.MappedRanges = veneerRanges;
    hleRequest.MappedRanges.push_back({returnRip, static_cast<size_t>(pageSize), true, false});
    hleRequest.MappedRanges.push_back({reinterpret_cast<std::uintptr_t>(callStack.Get()),
                                       static_cast<size_t>(pageSize), false, true});
    hleRequest.MappedRanges.insert(hleRequest.MappedRanges.end(), extraRanges.begin(), extraRanges.end());
    prepare(hleRequest, rsp);
    activeRanges = hleRequest.MappedRanges;
    const auto hleRunResult = hleBackend->Run(hleRequest);
    if (const auto* failure = std::get_if<Core::GuestExecutionFailure>(&hleRunResult)) {
      Fail(*failure);
      return false;
    }
    return verify(std::get<Core::GuestExecutionState>(hleRunResult));
  };

  Trace("hle-scalar-begin");
  const bool scalarOk = runHle(
      scalarVeneer,
      [](Core::GuestExecutionRequest& request, std::uintptr_t) {
        request.Gpr[FEXCore::X86State::REG_RDI] = 0x1122;
        request.Gpr[FEXCore::X86State::REG_RSI] = 0x3344;
      },
      [](const Core::GuestExecutionState& state) {
        return state.Gpr[FEXCore::X86State::REG_RAX] == 0x4466;
      });
  Trace("hle-scalar-end");
  if (!scalarOk || hleFailure.error != 0) {
    return Fail({Core::GuestExecutionStage::Execute,
                 hleFailure.error == 0 ? EPROTO : hleFailure.error});
  }

  Mapping pointerPage{static_cast<size_t>(pageSize)};
  if (!pointerPage.IsValid()) return Fail({Core::GuestExecutionStage::Mapping, ENOMEM});
  auto* pointerValue = static_cast<uint64_t*>(pointerPage.Get());
  *pointerValue = 41;
  const Core::GuestExecutionRange pointerRange{reinterpret_cast<std::uintptr_t>(pointerPage.Get()),
                                                static_cast<size_t>(pageSize), false, true};
  Trace("hle-pointer-begin");
  const bool pointerOk = runHle(
      pointerVeneer,
      [pointerValue](Core::GuestExecutionRequest& request, std::uintptr_t) {
        request.Gpr[FEXCore::X86State::REG_RDI] = reinterpret_cast<std::uintptr_t>(pointerValue);
        request.Gpr[FEXCore::X86State::REG_RSI] = 1;
      },
      [pointerValue](const Core::GuestExecutionState& state) {
        return state.Gpr[FEXCore::X86State::REG_RAX] == 42 && *pointerValue == 42;
      },
      std::span<const Core::GuestExecutionRange>{&pointerRange, 1});
  Trace("hle-pointer-end");
  if (!pointerOk || hleFailure.error != 0) {
    return Fail({Core::GuestExecutionStage::Execute,
                 hleFailure.error == 0 ? EPROTO : hleFailure.error});
  }

  constexpr uint64_t functionPointerValue = 0x1234'5678'9abc'def0ULL;
  const bool functionPointerOk = runHle(
      functionPointerVeneer,
      [](Core::GuestExecutionRequest& request, std::uintptr_t) {
        request.Gpr[FEXCore::X86State::REG_RDI] = functionPointerValue;
      },
      [](const Core::GuestExecutionState& state) {
        return state.Gpr[FEXCore::X86State::REG_RAX] == functionPointerValue;
      });
  if (!functionPointerOk || hleFailure.error != 0) {
    return Fail({Core::GuestExecutionStage::Execute,
                 hleFailure.error == 0 ? EPROTO : hleFailure.error});
  }

  Trace("hle-vector-begin");
  const bool vectorOk = runHle(
      vectorVeneer,
      [](Core::GuestExecutionRequest& request, std::uintptr_t) {
        request.Xmm[0][0] = DoubleBits(1.5);
        request.Xmm[1][0] = DoubleBits(4.0);
      },
      [](const Core::GuestExecutionState& state) {
        return state.Xmm[0][0] == DoubleBits(6.0);
      });
  Trace("hle-vector-end");
  if (!vectorOk || hleFailure.error != 0) {
    return Fail({Core::GuestExecutionStage::Execute,
                 hleFailure.error == 0 ? EPROTO : hleFailure.error});
  }

  Trace("hle-stack-begin");
  const bool stackOk = runHle(
      stackVeneer,
      [](Core::GuestExecutionRequest& request, std::uintptr_t rsp) {
        request.Gpr[FEXCore::X86State::REG_RDI] = 1;
        request.Gpr[FEXCore::X86State::REG_RSI] = 2;
        request.Gpr[FEXCore::X86State::REG_RDX] = 3;
        request.Gpr[FEXCore::X86State::REG_RCX] = 4;
        request.Gpr[FEXCore::X86State::REG_R8] = 5;
        request.Gpr[FEXCore::X86State::REG_R9] = 6;
        const uint64_t seventh = 7;
        std::memcpy(reinterpret_cast<void*>(rsp + sizeof(uint64_t)), &seventh, sizeof(seventh));
      },
      [](const Core::GuestExecutionState& state) {
        return state.Gpr[FEXCore::X86State::REG_RAX] == 28;
      });
  Trace("hle-stack-end");
  if (!stackOk || hleFailure.error != 0) {
    return Fail({Core::GuestExecutionStage::Execute, hleFailure.error == 0 ? EPROTO : hleFailure.error});
  }

  std::printf("FEXCORE_GUEST_ENGINE_OK revision=%s gpr=ok rflags=ok xmm=ok bridge=ok threads=ok tls=ok unaligned=ok invalidation=ok teardown=ok\n", kFexRevision);
  std::printf("FEXCORE_GUEST_CPU_OK caller_mapping=ok thread_lifetime=ok invalidation=ok thread_isolation=ok overlap_rejected=ok nested_callback=ok\n");
  std::printf("HLE_VENEER_OK scalar=ok pointer=ok function_pointer=ok vector=ok stack=ok mapping=ok\n");
  return 0;
}
