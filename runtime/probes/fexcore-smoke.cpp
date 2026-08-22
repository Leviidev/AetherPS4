// SPDX-License-Identifier: MIT

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>

#if defined(FEXCORE_SMOKE_DIAGNOSTIC)
#include <fstream>
#include <signal.h>
#include <string>
#include <ucontext.h>
#endif

#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#endif

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {
#if defined(__APPLE__)
// Apple Silicon's host VM subsystem is unconditionally 16KB-granularity (unlike Linux's
// usual 4KB) -- this constant only sizes the probe's own single-page code/stack/TLS
// mappings below, nothing here requires the literal value 4096, just internal consistency.
constexpr long kRequiredPageSize = 16384;
#else
constexpr long kRequiredPageSize = 4096;
#endif
constexpr uint64_t kAddLeft = 0x1122'3344'5566'7788ULL;
constexpr uint64_t kAddRight = 0x0102'0304'0506'0708ULL;
constexpr uint64_t kXorLeft = 0xfedc'ba98'7654'3210ULL;
constexpr uint64_t kXorRight = 0x0f0f'0f0f'0f0f'0f0fULL;
constexpr uint64_t kStackSentinel = 0xcafe'babe'dead'beefULL;
constexpr double kFpLeft = 1.5;
constexpr double kFpRight = 2.25;
constexpr double kFpResult = 3.75;
constexpr uint64_t kThreadSentinelA = 0x1357'9bdf'2468'ace0ULL;
constexpr uint64_t kThreadSentinelB = 0x0246'8ace'1357'9bdfULL;
constexpr uint64_t kCallbackInput = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kInvalidationInitial = 0x0123'4567'89ab'cdefULL;
constexpr uint64_t kInvalidationUpdated = 0xfedc'ba98'7654'3210ULL;
constexpr std::string_view kFexRevision =
  "f2b679f6028ce1c38875233aecfcf5d3f8ebecec";

#if defined(FEXCORE_SMOKE_DIAGNOSTIC)
void WriteDiagnostic(const char* message, size_t size) {
  while (size != 0) {
    const auto written = write(STDERR_FILENO, message, size);
    if (written <= 0) return;
    message += written;
    size -= static_cast<size_t>(written);
  }
}

void SmokeDiagnosticPreinit() {
  WriteDiagnostic("FEXCORE_DIAG:PREINIT\n", sizeof("FEXCORE_DIAG:PREINIT\n") - 1);
}

__attribute__((section(".preinit_array"), used))
void (*const kSmokeDiagnosticPreinit)() = SmokeDiagnosticPreinit;

bool IsDiagnosticExecutableMap(std::string_view line) {
  const auto firstSpace = line.find(' ');
  if (firstSpace == std::string_view::npos) return false;

  auto permissionsBegin = firstSpace + 1;
  while (permissionsBegin < line.size() && line[permissionsBegin] == ' ') {
    ++permissionsBegin;
  }
  const auto permissionsEnd = line.find(' ', permissionsBegin);
  if (permissionsEnd == std::string_view::npos) return false;
  return line.substr(permissionsBegin, permissionsEnd - permissionsBegin).find('x') != std::string_view::npos;
}

void DumpDiagnosticExecutableMaps() {
  std::ifstream maps {"/proc/self/maps"};
  std::string line;
  while (std::getline(maps, line)) {
    if (IsDiagnosticExecutableMap(line)) {
      std::fprintf(stderr, "FEXCORE_DIAG:MAP %s\n", line.c_str());
    }
  }
}

void AppendFaultLiteral(char*& cursor, const char* value) {
  while (*value != '\0') {
    *cursor++ = *value++;
  }
}

void AppendFaultUnsigned(char*& cursor, uint64_t value, uint64_t base) {
  char reversed[16];
  size_t count = 0;
  do {
    const auto digit = value % base;
    reversed[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    value /= base;
  } while (value != 0);
  while (count != 0) {
    *cursor++ = reversed[--count];
  }
}

void FaultDiagnosticHandler(int signal, siginfo_t* info, void* rawContext) {
  char message[128];
  char* cursor = message;
  const auto faultAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info == nullptr ? nullptr : info->si_addr));
  const auto pc = rawContext == nullptr
    ? 0ULL
    : static_cast<uint64_t>(reinterpret_cast<ucontext_t*>(rawContext)->uc_mcontext.pc);

  AppendFaultLiteral(cursor, "FEXCORE_DIAG:FAULT signal=");
  AppendFaultUnsigned(cursor, static_cast<uint64_t>(signal), 10);
  AppendFaultLiteral(cursor, " addr=0x");
  AppendFaultUnsigned(cursor, faultAddress, 16);
  AppendFaultLiteral(cursor, " pc=0x");
  AppendFaultUnsigned(cursor, pc, 16);
  *cursor++ = '\n';
  WriteDiagnostic(message, static_cast<size_t>(cursor - message));
  _exit(128 + signal);
}

bool InstallFaultDiagnosticHandler(int signal) {
  struct sigaction action {};
  action.sa_sigaction = FaultDiagnosticHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  return sigaction(signal, &action, nullptr) == 0;
}

bool InstallFaultDiagnosticHandlers() {
  return InstallFaultDiagnosticHandler(SIGSEGV) &&
         InstallFaultDiagnosticHandler(SIGILL) &&
         InstallFaultDiagnosticHandler(SIGBUS) &&
         InstallFaultDiagnosticHandler(SIGSYS);
}

#define FEXCORE_DIAG(marker) WriteDiagnostic("FEXCORE_DIAG:" marker "\n", sizeof("FEXCORE_DIAG:" marker "\n") - 1)
#define FEXCORE_DUMP_EXECUTABLE_MAPS() DumpDiagnosticExecutableMaps()
#define FEXCORE_INSTALL_FAULT_DIAGNOSTIC() InstallFaultDiagnosticHandlers()
#else
#define FEXCORE_DIAG(marker) ((void)0)
#define FEXCORE_DUMP_EXECUTABLE_MAPS() ((void)0)
#define FEXCORE_INSTALL_FAULT_DIAGNOSTIC() true
#endif

int Fail(std::string_view check) {
  std::fprintf(stderr, "FEXCORE_SMOKE_FAIL check=%.*s\n", static_cast<int>(check.size()), check.data());
  return 1;
}

class Mapping final {
public:
  Mapping(size_t size, int protection)
    : Size {size}
#ifdef __APPLE__
    // Darwin refuses any RWX/exec-capable anonymous mapping without MAP_JIT (EACCES
    // otherwise). This "code" page holds guest x86-64 bytes that FEXCore only ever reads
    // as data (the host CPU never fetches instructions from it -- FEXCore JITs guest code
    // into its own separate ARM64 code cache), but Darwin's mmap still enforces MAP_JIT for
    // any PROT_EXEC request regardless of what's actually stored there.
    , Address {mmap(nullptr, size, protection,
                     MAP_PRIVATE | MAP_ANONYMOUS | ((protection & PROT_EXEC) ? MAP_JIT : 0), -1, 0)} {
    // A freshly created MAP_JIT mapping starts in the write-protected (execute) state for
    // this thread; writes fault (SIGBUS) until write-protection is disabled. Since this page
    // is never host-executed, it's safe to leave write-protection off permanently rather
    // than toggle it around each write site.
    if (Address != MAP_FAILED && (protection & PROT_EXEC)) {
      pthread_jit_write_protect_np(0);
    }
  }
#else
    , Address {mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)} {}
#endif

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  ~Mapping() {
    if (Address != MAP_FAILED && munmap(Address, Size) != 0) {
      std::fprintf(stderr, "FEXCORE_SMOKE_FATAL check=release-mapping errno=%d\n", errno);
      std::abort();
    }
  }

  bool IsValid() const {
    return Address != MAP_FAILED;
  }

  void* Get() const {
    return Address;
  }

private:
  size_t Size;
  void* Address;
};

class CallRetStack final {
public:
  CallRetStack()
    : Address {mmap(nullptr, kAllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)} {}

  CallRetStack(const CallRetStack&) = delete;
  CallRetStack& operator=(const CallRetStack&) = delete;

  ~CallRetStack() {
    if (IsReserved() && munmap(Address, kAllocationSize) != 0) {
      std::fprintf(stderr, "FEXCORE_SMOKE_FATAL check=release-callret-stack errno=%d\n", errno);
      std::abort();
    }
  }

  bool IsReserved() const {
    return Address != MAP_FAILED;
  }

  bool MakeWritable() const {
    return mprotect(StackBase(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE) == 0;
  }

  void Initialize(FEXCore::Core::InternalThreadState* thread) const {
    thread->CallRetStackBase = StackBase();
    // FEX's documented default location is one quarter into the writable stack.
    thread->CurrentFrame->State.callret_sp =
      reinterpret_cast<uint64_t>(StackBase()) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  }

private:
  static constexpr size_t kAllocationSize = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * kRequiredPageSize;

  void* StackBase() const {
    return static_cast<uint8_t*>(Address) + kRequiredPageSize;
  }

  void* Address;
};

class ConfigScope final {
public:
  ConfigScope() {
    FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
  }

  ConfigScope(const ConfigScope&) = delete;
  ConfigScope& operator=(const ConfigScope&) = delete;

  ~ConfigScope() {
    FEXCore::Config::Shutdown();
  }
};

class SmokeSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  SmokeSyscallHandler() {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override {
    return 0;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, std::numeric_limits<uint64_t>::max(), true};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

class SmokeSignalDelegator final : public FEXCore::SignalDelegator {
public:
  explicit SmokeSignalDelegator(uintptr_t callbackReturn)
    : CallbackReturn {callbackReturn} {}

  uintptr_t GetThunkCallbackRET() const override {
    return CallbackReturn;
  }

private:
  uintptr_t CallbackReturn;
};

class ThreadScope final {
public:
  ThreadScope(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread)
    : Context {context}
    , Thread {thread} {}

  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;

  ~ThreadScope() {
    if (Thread != nullptr) {
      Context->DestroyThread(Thread);
    }
  }

private:
  FEXCore::Context::Context* Context;
  FEXCore::Core::InternalThreadState* Thread;
};

class GuestSegmentState final {
public:
  void Initialize(FEXCore::Core::CPUState& state) {
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = GDT.data();
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = GDT.data();
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* codeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(codeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(codeSegment, 0xF'FFFFU);
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*codeSegment);
    codeSegment->L = 1;
    codeSegment->D = 0;
  }

private:
  std::array<FEXCore::Core::CPUState::gdt_segment, 32> GDT {};
};

void AppendImmediate(std::vector<uint8_t>& code, uint64_t value) {
  const auto offset = code.size();
  code.resize(offset + sizeof(value));
  std::memcpy(code.data() + offset, &value, sizeof(value));
}

struct GuestCode final {
  std::vector<uint8_t> Bytes;
  size_t CallbackOffset;
  size_t CallbackReturnOffset;
  size_t InvalidationOffset;
  size_t InvalidationImmediateOffset;
  size_t ThreadOffset;
};

struct ThreadProof final {
  bool Created {};
  bool TlsRead {};
};

GuestCode BuildGuestCode() {
  GuestCode result;
  auto& code = result.Bytes;

  code.insert(code.end(), {0x48, 0xb8}); // mov rax, kAddLeft
  AppendImmediate(code, kAddLeft);
  code.insert(code.end(), {0x48, 0xbb}); // mov rbx, kAddRight
  AppendImmediate(code, kAddRight);
  code.insert(code.end(), {0x48, 0x01, 0xd8}); // add rax, rbx

  code.insert(code.end(), {0x48, 0xb9}); // mov rcx, kXorLeft
  AppendImmediate(code, kXorLeft);
  code.insert(code.end(), {0x48, 0xba}); // mov rdx, kXorRight
  AppendImmediate(code, kXorRight);
  code.insert(code.end(), {0x48, 0x31, 0xd1}); // xor rcx, rdx

  code.insert(code.end(), {0xf2, 0x0f, 0x58, 0xc1}); // addsd xmm0, xmm1

  code.push_back(0xe8); // call stack_test
  const size_t callDisplacementOffset = code.size();
  code.resize(code.size() + sizeof(int32_t));
  code.push_back(0xf4); // hlt

  const size_t stackTestOffset = code.size();
  code.insert(code.end(), {0x48, 0xba}); // mov rdx, kStackSentinel
  AppendImmediate(code, kStackSentinel);
  code.push_back(0x52); // push rdx
  code.insert(code.end(), {0x48, 0x8b, 0x34, 0x24}); // mov rsi, [rsp]
  code.push_back(0x5f); // pop rdi
  code.push_back(0xc3); // ret

  const auto displacement = static_cast<int32_t>(stackTestOffset - (callDisplacementOffset + sizeof(int32_t)));
  std::memcpy(code.data() + callDisplacementOffset, &displacement, sizeof(displacement));

  result.CallbackOffset = code.size();
  code.insert(code.end(), {0x48, 0x8d, 0x47, 0x05, 0xc3}); // lea rax, [rdi + 5]; ret

  result.CallbackReturnOffset = code.size();
  code.insert(code.end(), {0x0f, 0x3e}); // FEX CALLBACKRET instruction

  result.InvalidationOffset = code.size();
  code.insert(code.end(), {0x48, 0xb8}); // mov rax, kInvalidationInitial
  result.InvalidationImmediateOffset = code.size();
  AppendImmediate(code, kInvalidationInitial);
  code.push_back(0xf4); // hlt

  result.ThreadOffset = code.size();
  code.insert(code.end(), {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}); // mov rax, fs:[0]
  code.push_back(0xf4); // hlt
  return result;
}

ThreadProof ExecuteThreadTlsCheck(FEXCore::Context::Context* context, uint64_t rip, size_t pageSize, uint64_t sentinel) {
  ThreadProof proof;
  Mapping stackPage {pageSize, PROT_READ | PROT_WRITE};
  Mapping tlsPage {pageSize, PROT_READ | PROT_WRITE};
  if (!stackPage.IsValid() || !tlsPage.IsValid()) return proof;
  std::memcpy(tlsPage.Get(), &sentinel, sizeof(sentinel));

  GuestSegmentState guestSegmentState;
  CallRetStack callRetStack;
  if (!callRetStack.IsReserved() || !callRetStack.MakeWritable()) return proof;

  const uint64_t initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + pageSize - 16;
  auto* thread = context->CreateThread(rip, initialRsp);
  if (thread == nullptr) return proof;
  callRetStack.Initialize(thread);
  ThreadScope threadScope {context, thread};
  guestSegmentState.Initialize(thread->CurrentFrame->State);
  thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());

  context->ExecuteThread(thread);
  proof.Created = true;
  proof.TlsRead = thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] == sentinel &&
                  thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] == initialRsp;
  return proof;
}

uint64_t DoubleBits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
} // namespace

int main() {
  FEXCORE_DIAG("MAIN_ENTER");
  FEXCORE_DIAG("PAGE_SIZE_BEFORE");
  const long pageSize = sysconf(_SC_PAGESIZE);
  FEXCORE_DIAG("PAGE_SIZE_AFTER");
  if (pageSize != kRequiredPageSize) {
    std::fprintf(stderr, "FEXCORE_SMOKE_FAIL check=page-size expected=%ld actual=%ld\n", kRequiredPageSize, pageSize);
    return 2;
  }

  FEXCORE_DIAG("MAPPINGS_BEFORE");
  Mapping codePage {static_cast<size_t>(pageSize), PROT_READ | PROT_WRITE | PROT_EXEC};
  if (!codePage.IsValid()) return Fail("map-code");
  Mapping stackPage {static_cast<size_t>(pageSize), PROT_READ | PROT_WRITE};
  if (!stackPage.IsValid()) return Fail("map-stack");
  Mapping tlsPage {static_cast<size_t>(pageSize), PROT_READ | PROT_WRITE};
  if (!tlsPage.IsValid()) return Fail("map-tls");
  FEXCORE_DIAG("MAPPINGS_AFTER");

  FEXCORE_DIAG("GUEST_CODE_BEFORE");
  const auto guestCode = BuildGuestCode();
  if (guestCode.Bytes.size() > static_cast<size_t>(pageSize)) return Fail("guest-code-size");
  std::memcpy(codePage.Get(), guestCode.Bytes.data(), guestCode.Bytes.size());
  const uint64_t initialRip = reinterpret_cast<uint64_t>(codePage.Get());
  FEXCORE_DIAG("GUEST_CODE_AFTER");

  FEXCORE_DIAG("CONFIG_SCOPE_BEFORE");
  ConfigScope config;
  FEXCORE_DIAG("CONFIG_SCOPE_AFTER");
  FEXCORE_DIAG("HOST_FEATURES_BEFORE");
  auto hostFeatures = FEX::FetchHostFeatures();
  FEXCORE_DIAG("HOST_FEATURES_AFTER");
  FEXCORE_DIAG("CREATE_CONTEXT_BEFORE");
  auto context = FEXCore::Context::Context::CreateNewContext(hostFeatures);
  FEXCORE_DIAG("CREATE_CONTEXT_AFTER");
  if (!context) return Fail("create-context");
  FEXCORE_DIAG("SIGNAL_SYSCALL_BEFORE");
  SmokeSignalDelegator signalDelegator {initialRip + guestCode.CallbackReturnOffset};
  auto syscallHandler = fextl::make_unique<SmokeSyscallHandler>();

  context->SetSignalDelegator(&signalDelegator);
  context->SetSyscallHandler(syscallHandler.get());
  context->EnableExitOnHLT();
  FEXCORE_DIAG("SIGNAL_SYSCALL_AFTER");
  FEXCORE_DIAG("INIT_CORE_BEFORE");
  const bool initialized = context->InitCore();
  FEXCORE_DIAG("INIT_CORE_AFTER");
  if (!initialized) return Fail("init-core");

  const uint64_t initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + pageSize - 16;
  GuestSegmentState guestSegmentState;
  CallRetStack callRetStack;
  if (!callRetStack.IsReserved()) return Fail("reserve-callret-stack");
  if (!callRetStack.MakeWritable()) return Fail("protect-callret-stack");
  FEXCORE_DIAG("CREATE_THREAD_BEFORE");
  auto* thread = context->CreateThread(initialRip, initialRsp);
  FEXCORE_DIAG("CREATE_THREAD_AFTER");
  if (thread == nullptr) return Fail("create-thread");
  callRetStack.Initialize(thread);
  ThreadScope threadScope {context.get(), thread};
  guestSegmentState.Initialize(thread->CurrentFrame->State);
  thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());

  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialXmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialYmmHigh {};
  const uint64_t fpLeftBits = DoubleBits(kFpLeft);
  const uint64_t fpRightBits = DoubleBits(kFpRight);
  std::memcpy(&initialXmm[0], &fpLeftBits, sizeof(fpLeftBits));
  std::memcpy(&initialXmm[1], &fpRightBits, sizeof(fpRightBits));
  context->SetXMMRegistersFromState(thread, initialXmm.data(), hostFeatures.SupportsAVX ? initialYmmHigh.data() : nullptr);

  FEXCORE_DIAG("EXECUTE_BEFORE");
  FEXCORE_DUMP_EXECUTABLE_MAPS();
  if (!FEXCORE_INSTALL_FAULT_DIAGNOSTIC()) return Fail("install-fault-diagnostic");
  context->ExecuteThread(thread);
  FEXCORE_DIAG("EXECUTE_AFTER");

  auto& state = thread->CurrentFrame->State;
  if (state.gregs[FEXCore::X86State::REG_RAX] != kAddLeft + kAddRight ||
      state.gregs[FEXCore::X86State::REG_RCX] != (kXorLeft ^ kXorRight)) {
    return Fail("gpr");
  }
  if (state.gregs[FEXCore::X86State::REG_RSI] != kStackSentinel || state.gregs[FEXCore::X86State::REG_RDI] != kStackSentinel ||
      state.gregs[FEXCore::X86State::REG_RSP] != initialRsp) {
    return Fail("stack");
  }

  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalXmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalYmmHigh {};
  context->ReconstructXMMRegisters(thread, finalXmm.data(), hostFeatures.SupportsAVX ? finalYmmHigh.data() : nullptr);
  uint64_t fpResultBits;
  std::memcpy(&fpResultBits, &finalXmm[0], sizeof(fpResultBits));
  if (fpResultBits != DoubleBits(kFpResult)) return Fail("fp");

  ThreadProof firstThreadProof;
  ThreadProof secondThreadProof;
  const uint64_t threadRip = initialRip + guestCode.ThreadOffset;
  FEXCORE_DIAG("THREADS_BEFORE");
  std::thread firstHostThread {[&] {
    firstThreadProof = ExecuteThreadTlsCheck(context.get(), threadRip, static_cast<size_t>(pageSize), kThreadSentinelA);
  }};
  std::thread secondHostThread {[&] {
    secondThreadProof = ExecuteThreadTlsCheck(context.get(), threadRip, static_cast<size_t>(pageSize), kThreadSentinelB);
  }};
  firstHostThread.join();
  secondHostThread.join();
  FEXCORE_DIAG("THREADS_AFTER");
  if (!firstThreadProof.Created || !secondThreadProof.Created) return Fail("threads");
  if (!firstThreadProof.TlsRead || !secondThreadProof.TlsRead) return Fail("tls");

  const uint64_t callbackRsp = initialRsp;
  state.gregs[FEXCore::X86State::REG_RDI] = kCallbackInput;
  state.gregs[FEXCore::X86State::REG_RSP] = callbackRsp;
  FEXCORE_DIAG("CALLBACK_BEFORE");
  context->HandleCallback(thread, initialRip + guestCode.CallbackOffset);
  FEXCORE_DIAG("CALLBACK_AFTER");
  if (state.gregs[FEXCore::X86State::REG_RAX] != kCallbackInput + 5 ||
      state.gregs[FEXCore::X86State::REG_RSP] != callbackRsp) {
    return Fail("callback");
  }

  const uint64_t invalidationRip = initialRip + guestCode.InvalidationOffset;
  state.rip = invalidationRip;
  state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
  FEXCORE_DIAG("INVALIDATION_FIRST_BEFORE");
  context->ExecuteThread(thread);
  FEXCORE_DIAG("INVALIDATION_FIRST_AFTER");
  if (state.gregs[FEXCore::X86State::REG_RAX] != kInvalidationInitial) return Fail("invalidation-initial");

  auto* invalidationImmediate = static_cast<uint8_t*>(codePage.Get()) + guestCode.InvalidationImmediateOffset;
#ifdef __APPLE__
  // pthread_jit_write_protect_np is a *global per-thread* toggle, not per-mapping: the
  // Mapping constructor's one-time protect(0) call (for the earlier memcpy into this same
  // page) has since been silently undone by FEXCore's own internal write-protect toggling
  // during JIT compilation on this thread (CompileCode/ExitFunctionLink's ScopedJITWriteProtect
  // guards, which flip back to execute-enabled on every return) -- so this later write needs
  // its own explicit toggle rather than assuming the constructor's setting still holds.
  pthread_jit_write_protect_np(0);
#endif
  std::memcpy(invalidationImmediate, &kInvalidationUpdated, sizeof(kInvalidationUpdated));
  {
    std::scoped_lock codeInvalidationLock {context->GetCodeInvalidationMutex()};
    context->InvalidateCodeBuffersCodeRange(invalidationRip, 11);
    context->InvalidateThreadCachedCodeRange(thread, invalidationRip, 11);
  }
  FEXCORE_DIAG("INVALIDATION_FLUSH_AFTER");
  state.rip = invalidationRip;
  state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
  context->ExecuteThread(thread);
  FEXCORE_DIAG("INVALIDATION_SECOND_AFTER");
  if (state.gregs[FEXCore::X86State::REG_RAX] != kInvalidationUpdated) return Fail("invalidation");

  std::printf("FEXCORE_SMOKE_OK revision=%.*s gpr=ok stack=ok fp=ok threads=ok tls=ok callback=ok invalidation=ok\n", static_cast<int>(kFexRevision.size()),
              kFexRevision.data());
  return 0;
}
