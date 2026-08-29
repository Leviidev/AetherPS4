// SPDX-License-Identifier: MIT
//
// Runs FEXCore's own instruction-level correctness-test corpus (raw x86-64 machine code +
// expected-register-state JSON, see unittests/ASM/ in the vendored fexcore-darwin/fex source
// trees) directly against this fork's FEXCore build, bypassing upstream's TestHarnessRunner
// entirely.
//
// Why not just use TestHarnessRunner: it links LinuxEmulation (real Linux guest-syscall
// emulation -- clone(), seccomp/BPF, /proc, epoll, GdbServer) which doesn't exist on
// iOS/Darwin and isn't buildable here; this fork's whole BUILD_FEXCORE_ONLY strategy exists
// specifically to avoid needing it (see fexcore-darwin/README.aethercore-darwin.md). Instead
// this reuses the exact same minimal Context/Thread API fexcore-smoke.cpp already validates
// (CreateNewContext / CreateThread / ExecuteThread / ReconstructXMMRegisters), extended to
// load a directory of pre-assembled tests (see runtime/scripts/prepare-fex-asm-tests.py,
// which strips each test's %ifdef CONFIG JSON block and assembles the rest via nasm) instead
// of one hand-written test case.
//
// Rationale for existing at all: three separate PS4 games this fork tried to run each hit a
// completely different, unrelated-looking crash (a std::exit() thread-safety bug, unexplained
// guest heap corruption, a guest null-pointer write) -- discoverable only by manually playing
// a real game for anywhere from minutes to over an hour. FEXCore's own test corpus includes
// unittests/ASM/FEX_bugs/, real regression tests filed from exactly this kind of bug in other
// games historically (see e.g. nzcv_spill_enderlilies.asm, CodeBufferOverflow.asm) -- running
// it directly finds the same class of dynarec correctness bug in milliseconds per test instead
// of hoping a specific game's specific instruction sequence happens to trip it.

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <nlohmann/json.hpp>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {
// This machine's actual page size is 16384, not the 4096 fexcore-smoke.cpp hardcodes
// (confirmed via that probe's own page-size guard failing here) -- mprotect/mmap offsets
// computed against a wrong, smaller page size land on non-page-aligned addresses and fail.
// Query the real value instead of assuming.
long PageSize() {
  static const long size = sysconf(_SC_PAGESIZE);
  return size;
}

using Json = nlohmann::json;

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
  ~ConfigScope() { FEXCore::Config::Shutdown(); }
};

class AsmTestSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  AsmTestSyscallHandler() { OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC; }
  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override { return 0; }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, std::numeric_limits<uint64_t>::max(), true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

class AsmTestSignalDelegator final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override { return 0; }
};

// A guest-address-space mapping. Unlike fexcore-smoke.cpp's Mapping (always host-chosen
// address), this can also place memory at a specific guest address -- MemoryRegions test
// config entries are declared as fixed addresses (e.g. "0x100000000") the test's own code
// references directly, not relocatable.
class Mapping final {
public:
  Mapping(size_t size, int protection, void* fixedAddress = nullptr)
    : Size {size} {
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS | (fixedAddress != nullptr ? MAP_FIXED : 0) |
                       // This machine's hardened memory policy rejects a plain RWX anonymous
                       // mapping outright (EACCES) even for an unsigned/ad-hoc-signed process --
                       // confirmed via a standalone repro outside this codebase entirely, so
                       // it's an OS/toolchain policy, not something specific to this binary.
                       // MAP_JIT satisfies it; for a non-hardened-runtime process (this one) no
                       // pthread_jit_write_protect_np() toggling is needed on top of it.
                       ((protection & PROT_EXEC) != 0 ? MAP_JIT : 0);
    Address = mmap(fixedAddress, size, protection, flags, -1, 0);
    // FEXCore::Context::Context (created once, before any test runs) pre-reserves large
    // swathes of the low guest address space for its own VMM bookkeeping -- confirmed via
    // ENOMEM specifically (not EINVAL/EACCES) on a MAP_FIXED mmap at 0xe0000000, an address
    // upstream's own harness treats as ordinary scratch memory (HarnessHelpers.h). A fixed
    // mmap can't claim territory FEXCore already owns, but mprotect can just change that
    // existing reservation's permissions in place -- OwnsMapping stays false in this path so
    // the destructor doesn't munmap memory this object never allocated.
    if (Address == MAP_FAILED && fixedAddress != nullptr && errno == ENOMEM) {
      if (mprotect(fixedAddress, size, protection) == 0) {
        Address = fixedAddress;
        OwnsMapping = false;
      }
    }
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept : Size {other.Size}, Address {other.Address}, OwnsMapping {other.OwnsMapping} {
    other.Address = MAP_FAILED;
  }
  ~Mapping() {
    if (Address != MAP_FAILED && OwnsMapping) munmap(Address, Size);
  }
  bool IsValid() const { return Address != MAP_FAILED; }
  void* Get() const { return Address; }

private:
  size_t Size;
  void* Address = MAP_FAILED;
  bool OwnsMapping = true;
};

class ThreadScope final {
public:
  ThreadScope(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread)
    : Context {context}, Thread {thread} {}
  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;
  ~ThreadScope() {
    if (Thread != nullptr) Context->DestroyThread(Thread);
  }

private:
  FEXCore::Context::Context* Context;
  FEXCore::Core::InternalThreadState* Thread;
};

class CallRetStack final {
public:
  CallRetStack()
    : Address {mmap(nullptr, AllocationSize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)} {}
  CallRetStack(const CallRetStack&) = delete;
  CallRetStack& operator=(const CallRetStack&) = delete;
  ~CallRetStack() {
    if (IsReserved()) munmap(Address, AllocationSize());
  }
  bool IsReserved() const { return Address != MAP_FAILED; }
  bool MakeWritable() const {
    return mprotect(StackBase(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE) == 0;
  }
  void Initialize(FEXCore::Core::InternalThreadState* thread) const {
    thread->CallRetStackBase = StackBase();
    thread->CurrentFrame->State.callret_sp =
      reinterpret_cast<uint64_t>(StackBase()) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  }

private:
  size_t AllocationSize() const {
    return FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * static_cast<size_t>(PageSize());
  }
  void* StackBase() const { return static_cast<uint8_t*>(Address) + PageSize(); }
  void* Address;
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

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Config hex fields accept "41 42 43" (spaced byte pairs) or "0x4142...” (a single run of hex
// digits, optionally 0x-prefixed) interchangeably per unittests/Example.asm's own docs -- both
// forms observed in the real corpus. Strip whitespace and an optional leading "0x", then decode
// whatever remains as a flat sequence of hex byte pairs, written in that left-to-right order
// starting at the target address.
std::vector<uint8_t> ParseHexBytes(std::string text) {
  std::string cleaned;
  for (char c : text) {
    if (!std::isspace(static_cast<unsigned char>(c))) cleaned += c;
  }
  if (cleaned.size() >= 2 && cleaned[0] == '0' && (cleaned[1] == 'x' || cleaned[1] == 'X')) {
    cleaned = cleaned.substr(2);
  }
  if (cleaned.size() % 2 != 0) cleaned = "0" + cleaned;
  std::vector<uint8_t> bytes;
  bytes.reserve(cleaned.size() / 2);
  for (size_t i = 0; i < cleaned.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(cleaned.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

uint64_t ParseHexU64(const std::string& text) {
  return std::stoull(text, nullptr, 16);
}

struct RegisterIndex {
  enum class Kind { GPR, XMM, RIP, Unsupported } kind;
  int index;
};

RegisterIndex LookupRegister(const std::string& name) {
  static const std::map<std::string, int> kGprs = {
    {"RAX", FEXCore::X86State::REG_RAX}, {"RCX", FEXCore::X86State::REG_RCX},
    {"RDX", FEXCore::X86State::REG_RDX}, {"RBX", FEXCore::X86State::REG_RBX},
    {"RSP", FEXCore::X86State::REG_RSP}, {"RBP", FEXCore::X86State::REG_RBP},
    {"RSI", FEXCore::X86State::REG_RSI}, {"RDI", FEXCore::X86State::REG_RDI},
    {"R8", FEXCore::X86State::REG_R8},   {"R9", FEXCore::X86State::REG_R9},
    {"R10", FEXCore::X86State::REG_R10}, {"R11", FEXCore::X86State::REG_R11},
    {"R12", FEXCore::X86State::REG_R12}, {"R13", FEXCore::X86State::REG_R13},
    {"R14", FEXCore::X86State::REG_R14}, {"R15", FEXCore::X86State::REG_R15},
  };
  if (auto it = kGprs.find(name); it != kGprs.end()) return {RegisterIndex::Kind::GPR, it->second};
  if (name == "RIP") return {RegisterIndex::Kind::RIP, 0};
  if (name.size() >= 4 && name.rfind("XMM", 0) == 0) {
    const int idx = std::stoi(name.substr(3));
    return {RegisterIndex::Kind::XMM, idx};
  }
  return {RegisterIndex::Kind::Unsupported, 0};
}

enum class TestResult { Pass, Fail, Skip, Crash };

struct TestOutcome {
  TestResult result;
  std::string detail;
};

void Checkpoint(const std::string& name, const char* marker) {
  std::fprintf(stderr, "CKPT %s %s\n", name.c_str(), marker);
  std::fflush(stderr);
}

TestOutcome RunOneTest(FEXCore::Context::Context* context, const FEXCore::HostFeatures& hostFeatures, const std::string& name,
                        const std::vector<uint8_t>& code, const Json& config) {
  Checkpoint(name, "enter");
  if (config.contains("Env")) {
    return {TestResult::Skip, "Env config overrides not supported by this harness yet"};
  }
  if (config.contains("HostFeatures")) {
    for (const auto& feature : config["HostFeatures"]) {
      const std::string featureName = feature.get<std::string>();
      if (featureName == "AVX" && !hostFeatures.SupportsAVX) {
        return {TestResult::Skip, "host lacks AVX"};
      }
    }
  }

  const auto pageSize = static_cast<size_t>(PageSize());
  // TESTING HYPOTHESIS: guest memory (where x86 bytes live, to be decoded/translated -- never
  // directly executed by the ARM64 host CPU) shouldn't need host PROT_EXEC at all. FEXCore's
  // OWN separately-managed JIT output buffer is the only thing that needs MAP_JIT/dual-mapping;
  // this is what caused the write-permission crashes on tests whose data lives in the same page
  // as their code.
  Mapping codePage {pageSize, PROT_READ | PROT_WRITE};
  const int codeErrno = errno;
  Mapping stackPage {pageSize * 4, PROT_READ | PROT_WRITE};
  const int stackErrno = errno;
  if (!codePage.IsValid() || !stackPage.IsValid()) {
    std::ostringstream detail;
    detail << "failed to map code/stack (code_valid=" << codePage.IsValid() << " code_errno=" << codeErrno
           << " stack_valid=" << stackPage.IsValid() << " stack_errno=" << stackErrno << ")";
    return {TestResult::Skip, detail.str()};
  }
  if (code.size() > pageSize) return {TestResult::Skip, "test code exceeds one page"};
  Checkpoint(name, "mapped-code-stack");
  std::memcpy(codePage.Get(), code.data(), code.size());
  Checkpoint(name, "copied-code");

  std::vector<Mapping> extraRegions;
  // Two scratch regions the real upstream harness always maps for every 64-bit test, whether
  // or not its own MemoryRegions declares them (see HarnessHelpers.h's
  // HarnessCodeLoader::MapMemory) -- 21 of the 111 FEX_bugs tests reference 0xe0000000
  // addresses without ever declaring a MemoryRegions entry for them (e.g. Push.asm sets
  // rsp = 0xe0000010 directly). Best-effort only: confirmed via a standalone repro with no
  // FEXCore involved at all that this exact address range is unmappable on this host
  // (macOS/Apple Silicon) regardless of method (plain mmap, MAP_FIXED, or mprotect on an
  // already-reserved range all fail identically with ENOMEM) -- a genuine host-platform
  // limitation, not something any amount of harness-side fixing can work around. Tests that
  // actually need this region legitimately crash on this host; that's real, reported
  // information (see RunOneTestIsolated), not a bug in this harness.
  {
    Mapping scratch {static_cast<size_t>(pageSize) * 10, PROT_READ | PROT_WRITE, reinterpret_cast<void*>(0xe000'0000ULL)};
    if (scratch.IsValid()) extraRegions.push_back(std::move(scratch));
    Mapping sib8 {pageSize * 2, PROT_READ | PROT_WRITE, reinterpret_cast<void*>(0xe800'0000ULL - pageSize)};
    if (sib8.IsValid()) extraRegions.push_back(std::move(sib8));
  }
  if (config.contains("MemoryRegions")) {
    for (auto& [addrStr, sizeStr] : config["MemoryRegions"].items()) {
      const auto addr = ParseHexU64(addrStr);
      const auto size = std::stoull(sizeStr.get<std::string>());
      Mapping region {size, PROT_READ | PROT_WRITE, reinterpret_cast<void*>(addr)};
      if (!region.IsValid()) return {TestResult::Skip, "failed to map MemoryRegions entry " + addrStr};
      extraRegions.push_back(std::move(region));
    }
  }
  Checkpoint(name, "mapped-memory-regions");
  if (config.contains("MemoryData")) {
    for (auto& [addrStr, dataStr] : config["MemoryData"].items()) {
      const auto addr = ParseHexU64(addrStr);
      const auto bytes = ParseHexBytes(dataStr.get<std::string>());
      std::memcpy(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
    }
  }
  Checkpoint(name, "wrote-memory-data");

  const uint64_t initialRip = reinterpret_cast<uint64_t>(codePage.Get());
  const uint64_t initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + pageSize * 4 - 16;

  CallRetStack callRetStack;
  if (!callRetStack.IsReserved() || !callRetStack.MakeWritable()) return {TestResult::Skip, "failed to prep callret stack"};
  Checkpoint(name, "callret-stack-ready");

  auto* thread = context->CreateThread(initialRip, initialRsp);
  if (thread == nullptr) return {TestResult::Skip, "CreateThread failed"};
  Checkpoint(name, "thread-created");
  ThreadScope threadScope {context, thread};
  callRetStack.Initialize(thread);
  GuestSegmentState segmentState;
  segmentState.Initialize(thread->CurrentFrame->State);
  Checkpoint(name, "before-execute");

  context->ExecuteThread(thread);
  Checkpoint(name, "after-execute");

  auto& state = thread->CurrentFrame->State;
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  context->ReconstructXMMRegisters(thread, xmm.data(), hostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  Checkpoint(name, "reconstructed-xmm");

  if (!config.contains("RegData")) return {TestResult::Pass, ""};

  std::ostringstream mismatches;
  int mismatchCount = 0;
  int uncheckedCount = 0;
  for (auto& [regName, expected] : config["RegData"].items()) {
    const auto reg = LookupRegister(regName);
    if (reg.kind == RegisterIndex::Kind::Unsupported) {
      ++uncheckedCount;
      continue;
    }
    if (reg.kind == RegisterIndex::Kind::GPR) {
      const uint64_t expectedValue = ParseHexU64(expected.get<std::string>());
      const uint64_t actual = state.gregs[reg.index];
      if (actual != expectedValue) {
        mismatches << regName << " expected=" << std::hex << expectedValue << " actual=" << actual << std::dec << "; ";
        ++mismatchCount;
      }
    } else if (reg.kind == RegisterIndex::Kind::RIP) {
      const uint64_t expectedValue = ParseHexU64(expected.get<std::string>());
      if (state.rip != expectedValue) {
        mismatches << "RIP expected=" << std::hex << expectedValue << " actual=" << state.rip << std::dec << "; ";
        ++mismatchCount;
      }
    } else if (reg.kind == RegisterIndex::Kind::XMM) {
      if (reg.index < 0 || reg.index >= static_cast<int>(FEXCore::Core::CPUState::NUM_XMMS)) {
        ++uncheckedCount;
        continue;
      }
      uint64_t low = 0, high = 0;
      std::memcpy(&low, &xmm[reg.index], sizeof(low));
      std::memcpy(&high, reinterpret_cast<const uint8_t*>(&xmm[reg.index]) + sizeof(low), sizeof(high));
      const auto& expectedList = expected;
      const uint64_t expectedLow = ParseHexU64(expectedList.at(0).get<std::string>());
      const uint64_t expectedHigh = expectedList.size() > 1 ? ParseHexU64(expectedList.at(1).get<std::string>()) : 0;
      if (low != expectedLow || (expectedList.size() > 1 && high != expectedHigh)) {
        mismatches << regName << " expected=" << std::hex << expectedHigh << ":" << expectedLow << " actual=" << high << ":" << low
                   << std::dec << "; ";
        ++mismatchCount;
      }
    }
  }

  if (mismatchCount > 0) return {TestResult::Fail, mismatches.str()};
  if (uncheckedCount > 0) return {TestResult::Skip, "RegData referenced an unsupported register kind (MM/Flags/etc)"};
  return {TestResult::Pass, ""};
}

// Real dynarec correctness bugs (the whole reason this harness exists) can crash guest
// execution outright, not just produce a wrong register value -- confirmed on the very first
// test run (32bit_syscall took down the entire process with SIGSEGV inside ExecuteThread).
// Running each test in its own fork()'d child isolates that: a crash only kills the child, the
// parent still gets a result for it (Crash, not silence) and continues with the rest of the
// corpus.
//
// The child creates its OWN fresh FEXCore::Context::Context after forking, rather than
// inheriting one built once in the parent before any forking. That inherited-context version
// was tried first and produced a much higher, systematic crash rate (35/111, all "byte write
// Permission fault" on MAP_JIT pages, specifically on any test needing a second/relinked JIT
// compile -- loops, repeated blocks) despite FEXCore's own JIT code being genuinely
// MAP_JIT-aware (ScopedJITWriteProtect guards throughout JIT.cpp/Dispatcher.cpp, with comments
// describing this exact failure mode already fixed). The likely explanation: MAP_JIT's
// per-thread write-protect state is tied to the process that originally created the mapping,
// and does not carry over correctly to a forked child even though the mapping itself survives
// fork() via copy-on-write. Creating the context fresh in each child sidesteps that entirely,
// at the cost of paying InitCore() once per test instead of once total.
TestOutcome RunOneTestIsolated(const std::string& name, const std::vector<uint8_t>& code, const Json& config) {
  int pipeFds[2];
  if (pipe(pipeFds) != 0) return {TestResult::Skip, "pipe() failed"};

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipeFds[0]);
    close(pipeFds[1]);
    return {TestResult::Skip, "fork() failed"};
  }
  if (pid == 0) {
    // Child: never return through normal unwind (would double-run destructors/atexit
    // handlers shared with the parent's own copy of the same state) -- report and _exit().
    close(pipeFds[0]);

    ConfigScope configScope;
    auto hostFeatures = FEX::FetchHostFeatures();
    auto context = FEXCore::Context::Context::CreateNewContext(hostFeatures);
    TestOutcome outcome;
    if (!context) {
      outcome = {TestResult::Skip, "failed to create context"};
    } else {
      AsmTestSignalDelegator signalDelegator;
      auto syscallHandler = fextl::make_unique<AsmTestSyscallHandler>();
      context->SetSignalDelegator(&signalDelegator);
      context->SetSyscallHandler(syscallHandler.get());
      context->EnableExitOnHLT();
      if (!context->InitCore()) {
        outcome = {TestResult::Skip, "InitCore failed"};
      } else {
        outcome = RunOneTest(context.get(), hostFeatures, name, code, config);
      }
    }
    const int resultCode = static_cast<int>(outcome.result);
    std::string message(1, static_cast<char>(resultCode));
    message += outcome.detail;
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(message.size())) {
      const auto written = write(pipeFds[1], message.data() + off, message.size() - static_cast<size_t>(off));
      if (written <= 0) break;
      off += written;
    }
    close(pipeFds[1]);
    _exit(0);
  }

  close(pipeFds[1]);
  std::string received;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipeFds[0], buf, sizeof(buf))) > 0) {
    received.append(buf, static_cast<size_t>(n));
  }
  close(pipeFds[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (WIFSIGNALED(status)) {
    std::ostringstream detail;
    detail << "crashed with signal " << WTERMSIG(status);
    return {TestResult::Crash, detail.str()};
  }
  if (received.empty()) {
    return {TestResult::Crash, "child exited without reporting a result"};
  }
  const auto result = static_cast<TestResult>(received[0]);
  return {result, received.substr(1)};
}

std::vector<std::string> ReadManifest(const std::string& dir) {
  std::vector<std::string> names;
  std::ifstream f(dir + "/manifest.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty()) names.push_back(line);
  }
  return names;
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <prepared_test_dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const auto names = ReadManifest(dir);
  if (names.empty()) {
    std::fprintf(stderr, "no tests found in manifest\n");
    return 2;
  }

  int passed = 0, failed = 0, skipped = 0, crashed = 0;
  std::vector<std::string> failedNames;
  std::vector<std::string> crashedNames;
  for (const auto& name : names) {
    const auto code = ReadFile(dir + "/" + name + ".bin");
    std::ifstream jsonFile(dir + "/" + name + ".json");
    Json testConfig;
    jsonFile >> testConfig;

    const auto outcome = RunOneTestIsolated(name, code, testConfig);
    switch (outcome.result) {
    case TestResult::Pass:
      ++passed;
      std::printf("PASS %s\n", name.c_str());
      break;
    case TestResult::Fail:
      ++failed;
      failedNames.push_back(name);
      std::printf("FAIL %s: %s\n", name.c_str(), outcome.detail.c_str());
      break;
    case TestResult::Skip:
      ++skipped;
      std::printf("SKIP %s: %s\n", name.c_str(), outcome.detail.c_str());
      break;
    case TestResult::Crash:
      ++crashed;
      crashedNames.push_back(name);
      std::printf("CRASH %s: %s\n", name.c_str(), outcome.detail.c_str());
      break;
    }
    std::fflush(stdout);
  }

  std::printf("FEXCORE_ASM_TESTS_SUMMARY passed=%d failed=%d skipped=%d crashed=%d total=%zu\n", passed, failed, skipped, crashed,
              names.size());
  if (!failedNames.empty()) {
    std::printf("FAILED TESTS:");
    for (const auto& n : failedNames) std::printf(" %s", n.c_str());
    std::printf("\n");
  }
  if (!crashedNames.empty()) {
    std::printf("CRASHED TESTS:");
    for (const auto& n : crashedNames) std::printf(" %s", n.c_str());
    std::printf("\n");
  }
  return (failed > 0 || crashed > 0) ? 1 : 0;
}
