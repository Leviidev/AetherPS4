// SPDX-License-Identifier: MIT

#include "fex_guest_engine.h"
#include "common/boot_timer.h"

// Ucontext layout only — do not include pthread.h here. pthread → semaphore →
// assert → log → spdlog, which the FEXCore-only guest harness does not provide.
#include "core/libraries/kernel/threads/exception.h"
// For TryRecoverCorruptedGuestRsp's VMM-backed rsp/rbp validation (Core::Memory::Instance(),
// OrbisVirtualQueryInfo) -- same two headers signals.cpp already pulls in for the equivalent
// checks there.
#include "core/libraries/kernel/memory.h"
#include "core/memory.h"
#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

// Internal (non-public-API) header, needed for ContextImpl's full definition -- specifically
// its OnBufferReusedInPlace member (see that member's own comment, Context.h) that this file
// registers a callback into below, since only application-level code here tracks every live
// guest thread (Threads, Impl::Threads below) at all; FEXCore's own core keeps no such list.
#include "Interface/Context/Context.h"
// InternalThreadState only forward-declares LookupCache; need the full type for
// AcquireWriteLock()/ClearThreadLocalCaches() in the OnBufferReusedInPlace callback below.
#include "Interface/Core/LookupCache.h"

#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
// Darwin's <ucontext.h> hard-errors on the deprecated getcontext/setcontext/swapcontext
// declarations unless _XOPEN_SOURCE is defined; we only need the ucontext_t/mcontext_t
// *types* below, never those functions, but the header aborts for everyone without this.
#define _XOPEN_SOURCE 1
#include <ucontext.h>
// Accessor macros for arm_thread_state64_t (pc/sp/fp/lr): on Apple Silicon these fields
// are pointer-authentication-opaque and cannot be read/written as plain integers -- see
// <mach/arm/_structs.h>. On a plain (non-arm64e) arm64 process like this one, the thread
// state carries FLAGS_NO_PTRAUTH and the macros degrade to raw field access, so they're
// safe to use unconditionally here as get/set of the actual register value.
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// iOS dual-mapped JIT: see src/core/ios/ios_jit_allocator.h for full protocol docs.
#include "core/ios/ios_jit_allocator.h"
#else
// iOS's SDK hard-blocks this header entirely (#error mach_vm.h unsupported.) -- see
// ValidateHostMapping's TARGET_OS_IPHONE branch below for the fallout.
#include <mach/mach_vm.h>
#endif
#else
#include <ucontext.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <pthread.h>

// Weak fallback when exception.cpp is not linked (fexcore-guest-harness).
// Full shadPS4 provides the strong definition that reads g_curthread.
namespace Libraries::Kernel {
__attribute__((weak)) u64 FexCurrentGuestStackTop() noexcept {
  return 0;
}
} // namespace Libraries::Kernel
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AetherPS4::Fex {
namespace {

#if defined(__APPLE__)
// Apple Silicon's host VM subsystem is unconditionally 16KB-granularity (unlike Linux's
// usual 4KB). This engine doesn't yet emulate sub-host-page guest mmap semantics for real
// PS4/x86-64 guest code (that's real, separate future work once shadPS4 integration begins
// -- out of scope here per the explicit "do not attempt commercial games yet" milestone);
// for this generic FEX-guest-CPU harness, kRequiredPageSize only needs to match the actual
// host page size so mappings stay self-consistent.
constexpr long kRequiredPageSize = 16384;
#else
constexpr long kRequiredPageSize = 4096;
#endif
constexpr uint32_t kFexBlockTraceLimit = 256;
constexpr uint64_t kAddLeft = 0x1122'3344'5566'7788ULL;
constexpr uint64_t kAddRight = 0x0102'0304'0506'0708ULL;
constexpr uint64_t kXorLeft = 0xfedc'ba98'7654'3210ULL;
constexpr uint64_t kXorRight = 0x0f0f'0f0f'0f0f'0f0fULL;
constexpr uint64_t kStackSentinel = 0xaabb'ccdd'eeff'0011ULL;
constexpr uint64_t kUnalignedSentinel = 0x8877'6655'4433'2211ULL;
constexpr uint64_t kThreadSentinelA = 0x1111'2222'3333'4444ULL;
constexpr uint64_t kThreadSentinelB = 0x5555'6666'7777'8888ULL;
constexpr uint64_t kCallbackInput = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kInvalidationInitial = 0x0123'4567'89ab'cdefULL;
constexpr uint64_t kInvalidationUpdated = 0xfedc'ba98'7654'3210ULL;

std::atomic_uint32_t FexBlockTraceCount {};

// Forward-declare so ActiveFexExecution can hold the HLE bridge for deferred flush.
class BridgeSyscallHandler;

struct ActiveFexExecutionState final {
  FEXCore::Context::Context* Context {};
  FEXCore::Core::InternalThreadState* Thread {};
  FEXCore::SignalDelegator* SignalDelegator {};
  BridgeSyscallHandler* Syscalls {};
};

thread_local ActiveFexExecutionState ActiveFexExecution;

// See BachataGetHgsCheckpoint's declaration in the header for why this exists: every
// direct-I/O diagnostic tried inside HandleGuestSignal (fprintf, vsnprintf+write,
// hand-rolled formatting+write, even bare hardcoded write() calls past its very first
// statement) produced zero output on-device past a certain point, despite the function
// provably running to completion (no crash follows). A plain relaxed atomic store touches
// no I/O and can't hang or fault, so it can't fall prey to whatever is silencing this
// function's other diagnostics -- whatever that turns out to be.
// clang-format off
enum class HgsCheckpoint : int {
  Idle = 0, Enter = 1, BailoutTaken = 2, PastBailoutCheck = 3, HavePc = 4,
  CheckedCodeBuffer = 5, NotInCodeBufferReturn = 6, IsAlignmentFault = 7,
  NotAlignmentFaultReturn = 8, BeforeWriteGuard = 9, AfterWriteGuard = 10,
  HaveWritablePc = 11, AfterHandleUnaligned = 12, AdjustmentFailedReturn = 13,
  SuccessReturnTrue = 14,
};
// clang-format on
std::atomic<int> g_hgs_checkpoint {static_cast<int>(HgsCheckpoint::Idle)};
void SetHgsCheckpoint(HgsCheckpoint value) noexcept {
  g_hgs_checkpoint.store(static_cast<int>(value), std::memory_order_relaxed);
}

// Safepoint mechanism backing ContextImpl::BeginBufferInvalidationSafepoint/
// EndBufferInvalidationSafepoint (see their own comment, Context.h): pauses every OTHER live
// guest thread via an async signal before a thread delinks/clears/reuses JIT code memory those
// threads might currently be mid-execution through via an already-resolved direct branch --
// confirmed on-device as a real, if low-probability, cause of "not tracked" crashes once
// Rocket League reached genuine multi-threaded rendering (two threads faulting into the same
// stale dispatcher region at nearly the same instant, right as a third thread's buffer-reuse
// cycle was in flight).
//
// SIGINFO is a real BSD/Darwin signal (historically the terminal "status" request, Ctrl+T) that
// this codebase's extensive PS4-guest signal mapping (core/libraries/kernel/threads/exception.cpp)
// does not claim for anything guest-visible -- checked every case there before picking it, so
// this cannot collide with a game installing its own handler for a signal number it expects to
// mean something on real hardware.
constexpr int kSafepointSignal = SIGINFO;
std::atomic<int> g_safepoint_paused_count {0};
std::atomic<bool> g_safepoint_resume {false};
std::atomic<bool> g_safepoint_handler_installed {false};
std::atomic<int> g_threads_in_hle_syscall {0};

// Wired into ContextImpl::ReuseGenerationCounter below and incremented once per completed
// reuse-in-place cycle, right as EndBufferInvalidationSafepoint releases the pause. See
// CpuStateFrame::ExitFunctionLinkValidatedGeneration's own comment (CoreState.h) for the full
// story: the safepoint signal above protects any thread it successfully pauses before a reuse
// starts, but signal delivery has real, nonzero latency -- a thread whose own trip through
// ExitFunctionLinker's post-call window happens to finish faster than that latency can execute
// a stale branch before ever being signaled at all. This counter is what ExitFunctionLinker
// checks itself, live, right before trusting a returned HostCode, as a fallback for exactly
// that gap -- confirmed on-device as the actual remaining source of the reuse-in-place crash
// even with the signal-based redirect already covering this same range.
std::atomic<uint64_t> g_reuse_generation {0};

// Rocket League's Game:Main thread was observed running normally for ~1.6s after guest entry
// (thousands of unaligned-access fixups, i.e. real JIT'd guest code executing), then going
// completely silent for the rest of a 7+ minute session -- no more fixups, no unsupported-HLE
// invocations (see UnsupportedHleCallAdapter's own trace), no draw calls, no crash. That
// pattern -- not slow, just gone quiet -- is what a thread parked in a single long/indefinite
// HLE call looks like from the outside, and there was no visibility into *which* call. This
// watchdog gives that visibility cheaply: every HandleSyscall records which operation it's
// about to invoke and when, and a background thread periodically checks whether that same
// operation has been in flight unreasonably long. Deliberately NOT signal-based (unlike the
// safepoint mechanism above) -- this only ever reads plain atomics from its own thread, so it
// can't perturb the JIT/threading correctness work that's already gone into this file.
std::atomic<uint64_t> g_last_hle_operation {0};
std::atomic<int64_t> g_last_hle_operation_start_ms {0};
std::atomic<int64_t> g_last_hle_operation_logged_start_ms {-1};
std::atomic<bool> g_hle_stall_watchdog_started {false};

// Second, identically-structured slot for RenderingThread -- added after Rocket League was
// confirmed (via BACHATA_SAFEPOINT thread lists and manual log inspection) to spawn a real
// UE4 RenderingThread that went silent at the exact same point Game:Main did, right after
// engine init finished ("Initializing Engine..." in the log) and before the first tick. The
// original single-slot design deliberately only watched Game:Main (see the comment below) to
// avoid worker threads clobbering it; RenderingThread is, like Game:Main, a single well-known
// thread worth its own dedicated slot for the same reason, not one of the many interchangeable
// pool workers.
std::atomic<uint64_t> g_last_hle_operation_rt {0};
std::atomic<int64_t> g_last_hle_operation_start_ms_rt {0};
std::atomic<int64_t> g_last_hle_operation_logged_start_ms_rt {-1};

void HleStallWatchdogThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    for (int which = 0; which < 2; ++which) {
      auto& start_ms_slot = which == 0 ? g_last_hle_operation_start_ms : g_last_hle_operation_start_ms_rt;
      auto& logged_slot =
        which == 0 ? g_last_hle_operation_logged_start_ms : g_last_hle_operation_logged_start_ms_rt;
      auto& op_slot = which == 0 ? g_last_hle_operation : g_last_hle_operation_rt;
      const char* who = which == 0 ? "Game:Main" : "RenderingThread";

      const auto start_ms = start_ms_slot.load(std::memory_order_acquire);
      if (start_ms == 0) {
        continue;
      }
      const auto elapsed_ms = Common::BootElapsedMs() - start_ms;
      // 3s: comfortably above any legitimate single HLE call's normal cost (even the slowest
      // ones logged elsewhere in this codebase, e.g. GetHostPath's directory scans, finish in
      // well under a second), so this only fires for a call that's genuinely stuck, not merely
      // busy.
      if (elapsed_ms < 3000) {
        continue;
      }
      // Dedup by start_ms, not a bool: a *different* call stalling later (new start_ms) after an
      // earlier one was already logged should still get its own log line.
      auto already_logged = logged_slot.load(std::memory_order_acquire);
      if (already_logged == start_ms) {
        continue;
      }
      logged_slot.store(start_ms, std::memory_order_release);
      LogMan::Msg::EFmt("BACHATA_HLE_STALL: thread={} operation={} has been in-flight for {}ms "
                        "without returning -- cross-reference against this log's own "
                        "BACHATA_FEX_VENEER lines for this operation number to find its name",
                        who, op_slot.load(std::memory_order_acquire), elapsed_ms);
    }
  }
}

void EnsureHleStallWatchdogStarted() {
  bool expected = false;
  if (!g_hle_stall_watchdog_started.compare_exchange_strong(expected, true)) {
    return;
  }
  std::thread(HleStallWatchdogThread).detach();
}

// g_last_hle_operation/_start_ms above are single global slots, not per-thread -- with this
// game's 13+ worker threads all making their own HLE calls concurrently (see the safepoint
// mechanism's own findings), any of their fast, routine calls would constantly overwrite
// whatever Game:Main's own slow/stuck call had recorded, hiding exactly the stall this
// watchdog exists to catch. Cheaper than per-thread bookkeeping for the one thread actually
// worth watching: only ever write these globals from Game:Main itself. thread_local caches
// the pthread_getname_np() lookup (set once, in linker.cpp's RunMainEntry, before any guest
// code -- including HLE calls -- ever runs) so this costs one string compare on this thread's
// first HLE call and nothing on every call after.
bool IsCurrentThreadGameMain() {
  thread_local int cached = -1;
  if (cached < 0) {
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    cached = (std::strcmp(name, "Game:Main") == 0) ? 1 : 0;
  }
  return cached != 0;
}

// See g_last_hle_operation_rt's own comment for why this thread gets the same treatment as
// Game:Main. Unlike Game:Main (renamed from a fixed name at guest-thread creation, checked once
// before any guest code runs), RenderingThread is a name UE4 itself assigns to a pthread it
// spawns partway through boot -- this can't cache a permanent answer on a thread's very first
// HLE call the way IsCurrentThreadGameMain does, since the *name itself* isn't set yet at that
// point for other threads that later rename themselves. Re-checking the name (cheap: one
// pthread_getname_np, no allocation) on every call is the correct tradeoff here.
bool IsCurrentThreadRenderingThread() {
  char name[64] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  return std::strcmp(name, "RenderingThread") == 0;
}

void SafepointSignalHandler(int, siginfo_t*, void* rawContext) noexcept {
  // If this thread was interrupted inside either of two lock-free dispatcher routines, it may
  // already have a soon-to-be-reused host code address loaded into a register with the branch
  // to it still pending:
  //  - DispatcherLoopTop's block-cache lookup (Dispatcher.cpp): reads a cached guest->host
  //    mapping straight out of L1/L2 with no synchronization at all -- the single hottest path
  //    in the entire JIT, executed on every guest dispatch.
  //  - ExitFunctionLinker (Dispatcher.cpp): calls into the C++ ExitFunctionLink (JIT.cpp,
  //    already re-validates the value it hands back under CodeInvalidationMutex before
  //    returning), then takes that return value in TMP1 and branches straight to it a few
  //    instructions later with no lock or synchronization of its own -- a second instance of
  //    the same hazard, just in the assembly glue around the call instead of inside it.
  // Neither routine's lock-free design is itself the bug -- pausing a thread mid-lookup is
  // exactly as safe as pausing it mid-execution of an already-linked branch, PROVIDED nothing
  // it's already holding in a register goes stale while it's frozen. Every other gap this
  // safepoint mechanism closes protects *memory* (delinking branches, clearing lookup caches)
  // or *ordering* (CodeInvalidationMutex acquired before the pause opens) -- none of that
  // reaches a value already sitting in a register. Confirmed on-device as the remaining source
  // of the "instruction-fetch fault, not inside any currently-live JIT allocation" crash even
  // after every other known gap in this mechanism was fixed: both faulting threads' saved link
  // registers pointed into the dispatcher's own tiny, fixed code range, and their actual fault
  // PCs were addresses the LookupCache had already forgotten -- exactly what resuming with a
  // stale cached address and then executing the pending branch produces. Redirecting PC back to
  // DispatcherLoopTop is safe from anywhere in either range: DispatcherLoopTop's own routine has
  // no observable side effect before its final branch, so redoing it from scratch loses nothing,
  // and ExitFunctionLink (JIT.cpp) now sets Frame->State.rip to the real target before every
  // return specifically so a DispatcherLoopTop redirect from within ExitFunctionLinker's range
  // lands on the correct guest RIP instead of whatever RIP was active before this call began.
  if (ActiveFexExecution.Thread != nullptr && rawContext != nullptr) {
    auto& Ptrs = ActiveFexExecution.Thread->CurrentFrame->Pointers;
    auto* context = reinterpret_cast<ucontext_t*>(rawContext);
    auto& ts = context->uc_mcontext->__ss;
    const auto pc = arm_thread_state64_get_pc(ts);
    const bool InBlockLookup =
      Ptrs.DispatcherBlockLookupRangeEnd != 0 && pc >= Ptrs.DispatcherLoopTop && pc < Ptrs.DispatcherBlockLookupRangeEnd;
    const bool InExitFunctionLinker =
      Ptrs.ExitFunctionLinkerRangeEnd != 0 && pc >= Ptrs.ExitFunctionLinker && pc < Ptrs.ExitFunctionLinkerRangeEnd;
    if (InBlockLookup || InExitFunctionLinker) {
      arm_thread_state64_set_pc_fptr(ts, reinterpret_cast<void*>(Ptrs.DispatcherLoopTop));
    }
  }

  g_safepoint_paused_count.fetch_add(1, std::memory_order_acq_rel);
  while (!g_safepoint_resume.load(std::memory_order_acquire)) {
    sched_yield();
  }
  g_safepoint_paused_count.fetch_sub(1, std::memory_order_acq_rel);
}

void EnsureSafepointHandlerInstalled() noexcept {
  bool expected = false;
  if (!g_safepoint_handler_installed.compare_exchange_strong(expected, true)) {
    return;
  }
  struct sigaction action {};
  action.sa_sigaction = SafepointSignalHandler;
  sigemptyset(&action.sa_mask);
  // Deliberately no SA_RESTART: a thread parked in a blocking host syscall isn't executing
  // guest code and so isn't at risk from a buffer reuse anyway (see the bounded-wait comment
  // at the call site below) -- but if it DOES get interrupted, EINTR is the normal, already-
  // handled outcome elsewhere in this codebase, safer than silently swallowing the signal
  // until the syscall eventually returns on its own. SA_SIGINFO is required now (rather than
  // the plain sa_handler this used before) to reach the interrupted thread's saved PC -- see
  // the handler's own comment for why.
  action.sa_flags = SA_SIGINFO;
  sigaction(kSafepointSignal, &action, nullptr);
}

struct PendingOrbisSignalState final {
  std::uintptr_t Handler {};
  int OrbisSig {};
  bool Pending {};
  bool Flushing {};
  // Host aarch64 snapshot at kill time so we can spill SRA → guest GPRs before
  // HandleCallback (mid-JIT live state is NOT in CurrentFrame).
  bool HasHostSnapshot {};
  std::uintptr_t HostPc {};
  std::array<std::uint64_t, 31> HostGprs {};
};
thread_local PendingOrbisSignalState PendingOrbisSignal {};

class FexExecutionSignalScope final {
public:
  FexExecutionSignalScope(FEXCore::Context::Context& context,
                          FEXCore::Core::InternalThreadState* thread,
                          FEXCore::SignalDelegator* signalDelegator,
                          BridgeSyscallHandler* syscalls = nullptr)
    : Previous {ActiveFexExecution} {
    ActiveFexExecution = {&context, thread, signalDelegator, syscalls};
  }

  FexExecutionSignalScope(const FexExecutionSignalScope&) = delete;
  FexExecutionSignalScope& operator=(const FexExecutionSignalScope&) = delete;

  ~FexExecutionSignalScope() {
    ActiveFexExecution = Previous;
  }

private:
  ActiveFexExecutionState Previous;
};

void FexMessageHandler(LogMan::DebugLevels level, const char* message) {
  if (message == nullptr) return;
  if (std::strstr(message, "Guest x86 Begin") != nullptr) {
    // Kept capped separately from everything else below: this class of message alone can be
    // one line per translated x86 block, unbounded-large over a real play session in a way
    // ordinary Info/Debug logging never is.
    const auto index = FexBlockTraceCount.fetch_add(1, std::memory_order_relaxed);
    if (index < kFexBlockTraceLimit) {
      std::fprintf(stderr, "BACHATA_FEX_BLOCK index=%u %s\n", index, message);
    }
    return;
  }
  if (level <= LogMan::ERROR) {
    std::fprintf(stderr, "BACHATA_FEX_ERROR level=%u %s\n", static_cast<unsigned>(level),
                 message);
    return;
  }
  // Forward everything else (Debug/Info) too while actively debugging -- previously this
  // silently dropped every FEXCore-internal message except Errors and two hand-picked JIT
  // keywords, which is exactly what hid the FEXCore-side half of the JIT allocation timeline
  // for most of this investigation. See emulator_settings.h's LogSettings::filter for the
  // equivalent "just capture everything" change on Bachata's own LOG_* call sites.
  std::fprintf(stderr, "BACHATA_FEX_MSG level=%u %s\n", static_cast<unsigned>(level), message);
}

EngineFailure Failure(EngineStage stage, int error) {
  return {stage, error == 0 ? EIO : error};
}

bool Contains(const Core::GuestExecutionRange& range, std::uintptr_t address, std::size_t size) {
  if (range.Begin == 0 || range.Size == 0 || size == 0 || address < range.Begin) {
    return false;
  }
  const auto rangeEnd = range.Begin + range.Size;
  const auto addressEnd = address + size;
  return rangeEnd >= range.Begin && addressEnd >= address && addressEnd <= rangeEnd;
}

bool RangesOverlap(const Core::GuestExecutionRange& lhs, const Core::GuestExecutionRange& rhs) {
  const auto lhsEnd = lhs.Begin + lhs.Size;
  const auto rhsEnd = rhs.Begin + rhs.Size;
  return lhsEnd >= lhs.Begin && rhsEnd >= rhs.Begin && lhs.Begin < rhsEnd && rhs.Begin < lhsEnd;
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
// iOS's SDK hard-blocks mach_vm.h entirely (#error mach_vm.h unsupported.) -- not a
// portability gap, a deliberate platform restriction, same as CodeCache.cpp's mach_vm_remap
// use elsewhere. This check is a defensive "fail fast with a clear error" diagnostic, not
// something correctness-critical to actual guest execution (if a mapping really is wrong,
// guest code will just fault instead of getting this earlier, more specific error) -- skipped
// outright on iOS rather than reimplemented, since there's no equivalent introspection API
// available here.
EngineResult<bool> ValidateHostMapping(const Core::GuestExecutionRange&) {
  return true;
}
#elif defined(__APPLE__)
// /proc doesn't exist on Darwin at all, so the host-mapping introspection below uses
// mach_vm_region() instead of parsing /proc/self/maps -- same idea (walk the regions
// covering the requested range, checking each one's actual host protection), just via the
// Mach VM API. mach_vm_region() looks up the region *containing* the given address; if that
// address falls in an unmapped gap, it advances the address to the next real region instead
// of failing, which is how the "gap" case below is detected (region start ends up past
// `covered`).
EngineResult<bool> ValidateHostMapping(const Core::GuestExecutionRange& range) {
  const auto rangeEnd = range.Begin + range.Size;
  std::uintptr_t covered = range.Begin;
  while (covered < rangeEnd) {
    mach_vm_address_t regionAddress = static_cast<mach_vm_address_t>(covered);
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;
    const kern_return_t kr = mach_vm_region(mach_task_self(), &regionAddress, &regionSize, VM_REGION_BASIC_INFO_64,
                                            reinterpret_cast<vm_region_info_t>(&info), &infoCount, &objectName);
    if (kr != KERN_SUCCESS || static_cast<std::uintptr_t>(regionAddress) > covered) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=host_mapping_gap error=%d begin=%#lx size=%#lx "
                   "executable=%d writable=%d covered=%#lx\n",
                   EFAULT, static_cast<unsigned long>(range.Begin), static_cast<unsigned long>(range.Size), range.Executable,
                   range.Writable, static_cast<unsigned long>(covered));
      return Failure(EngineStage::Mapping, EFAULT);
    }

    const bool hostReadable = (info.protection & VM_PROT_READ) != 0;
    const bool hostWritable = (info.protection & VM_PROT_WRITE) != 0;
    const bool hostExecutable = (info.protection & VM_PROT_EXECUTE) != 0;
    // Same policy as the Linux permission-string check below: guest-executable ranges must be
    // host-readable and not host-writable; guest-writable ranges must be host-writable and not
    // host-executable.
    if (range.Executable && (!hostReadable || hostWritable)) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=host_guest_code_permission error=%d "
                   "begin=%#lx size=%#lx executable=%d writable=%d host_protection=%#x\n",
                   EACCES, static_cast<unsigned long>(range.Begin), static_cast<unsigned long>(range.Size), range.Executable,
                   range.Writable, info.protection);
      return Failure(EngineStage::Mapping, EACCES);
    }
    if (range.Writable && (!hostWritable || hostExecutable)) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=host_write_permission error=%d begin=%#lx "
                   "size=%#lx executable=%d writable=%d host_protection=%#x\n",
                   EACCES, static_cast<unsigned long>(range.Begin), static_cast<unsigned long>(range.Size), range.Executable,
                   range.Writable, info.protection);
      return Failure(EngineStage::Mapping, EACCES);
    }
    covered = std::min(static_cast<std::uintptr_t>(regionAddress) + static_cast<std::uintptr_t>(regionSize), rangeEnd);
  }
  return true;
}
#else
EngineResult<bool> ValidateHostMapping(const Core::GuestExecutionRange& range) {
  std::ifstream maps {"/proc/self/maps"};
  if (!maps.is_open()) {
    const auto error = errno == 0 ? EACCES : errno;
    std::fprintf(stderr,
                 "BACHATA_FEX_MAPPING_FAIL reason=proc_maps_open error=%d begin=%#lx size=%#lx "
                 "executable=%d writable=%d\n",
                 error, static_cast<unsigned long>(range.Begin),
                 static_cast<unsigned long>(range.Size), range.Executable, range.Writable);
    return Failure(EngineStage::Mapping, error);
  }

  const auto rangeEnd = range.Begin + range.Size;
  std::uintptr_t covered = range.Begin;
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long mapBegin {};
    unsigned long mapEnd {};
    char permissions[5] {};
    if (std::sscanf(line.c_str(), "%lx-%lx %4s", &mapBegin, &mapEnd, permissions) != 3) continue;
    const auto begin = static_cast<std::uintptr_t>(mapBegin);
    const auto end = static_cast<std::uintptr_t>(mapEnd);
    if (end <= covered) continue;
    if (begin > covered) break;
    // FEX translates guest instructions into its own executable code cache. Guest code only
    // needs to be readable on the host, and must be sealed against writes before translation.
    if (range.Executable && (permissions[0] != 'r' || permissions[1] == 'w')) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=host_guest_code_permission error=%d "
                   "begin=%#lx size=%#lx executable=%d writable=%d host_begin=%#lx "
                   "host_end=%#lx host_permissions=%s\n",
                   EACCES, static_cast<unsigned long>(range.Begin),
                   static_cast<unsigned long>(range.Size), range.Executable, range.Writable,
                   static_cast<unsigned long>(begin), static_cast<unsigned long>(end), permissions);
      return Failure(EngineStage::Mapping, EACCES);
    }
    if (range.Writable && (permissions[1] != 'w' || permissions[2] == 'x')) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=host_write_permission error=%d begin=%#lx "
                   "size=%#lx executable=%d writable=%d host_begin=%#lx host_end=%#lx "
                   "host_permissions=%s\n",
                   EACCES, static_cast<unsigned long>(range.Begin),
                   static_cast<unsigned long>(range.Size), range.Executable, range.Writable,
                   static_cast<unsigned long>(begin), static_cast<unsigned long>(end), permissions);
      return Failure(EngineStage::Mapping, EACCES);
    }
    covered = std::min(end, rangeEnd);
    if (covered == rangeEnd) return true;
  }
  std::fprintf(stderr,
               "BACHATA_FEX_MAPPING_FAIL reason=host_mapping_gap error=%d begin=%#lx size=%#lx "
               "executable=%d writable=%d covered=%#lx\n",
               EFAULT, static_cast<unsigned long>(range.Begin),
               static_cast<unsigned long>(range.Size), range.Executable, range.Writable,
               static_cast<unsigned long>(covered));
  return Failure(EngineStage::Mapping, EFAULT);
}
#endif

EngineResult<bool> ValidateRequest(const Core::GuestExecutionRequest& request) {
  if (request.Rip == 0 || request.Rsp == 0 || request.MappedRanges.empty()) {
    return Failure(EngineStage::Request, EINVAL);
  }

  bool executableRip = false;
  bool writableStack = false;
  for (size_t index = 0; index < request.MappedRanges.size(); ++index) {
    const auto& range = request.MappedRanges[index];
    if (range.Begin == 0 || range.Size == 0 || range.Begin % kRequiredPageSize != 0 ||
        range.Size % kRequiredPageSize != 0 || range.Begin + range.Size < range.Begin) {
      return Failure(EngineStage::Request, EINVAL);
    }
    if (range.Executable && range.Writable) {
      std::fprintf(stderr,
                   "BACHATA_FEX_MAPPING_FAIL reason=guest_wx error=%d index=%zu begin=%#lx "
                   "size=%#lx\n",
                   EACCES, index, static_cast<unsigned long>(range.Begin),
                   static_cast<unsigned long>(range.Size));
      return Failure(EngineStage::Mapping, EACCES);
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (RangesOverlap(range, request.MappedRanges[previous])) {
        const auto& prior = request.MappedRanges[previous];
        std::fprintf(stderr,
                     "BACHATA_FEX_MAPPING_FAIL reason=guest_overlap error=%d previous=%zu "
                     "previous_begin=%#lx previous_size=%#lx index=%zu begin=%#lx size=%#lx\n",
                     EACCES, previous, static_cast<unsigned long>(prior.Begin),
                     static_cast<unsigned long>(prior.Size), index,
                     static_cast<unsigned long>(range.Begin),
                     static_cast<unsigned long>(range.Size));
        return Failure(EngineStage::Mapping, EACCES);
      }
    }
    const auto hostMapping = ValidateHostMapping(range);
    if (const auto* failure = std::get_if<EngineFailure>(&hostMapping)) return *failure;
    executableRip |= range.Executable && Contains(range, request.Rip, 1);
    writableStack |= range.Writable && Contains(range, request.Rsp, 1);
  }
  if (!executableRip || !writableStack) {
    return Failure(EngineStage::Request, EFAULT);
  }
  return true;
}

class FexConfigLease final {
public:
  static EngineResult<bool> Acquire() {
    std::scoped_lock lock {Mutex};
    if (Users == 0) {
      FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
      FEXCore::Config::Initialize();
      FEXCore::Config::Load();
      FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
      FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
      const bool traceEnabled = std::getenv("BACHATA_FEX_TRACE") != nullptr;
      FEXCore::Config::Set(FEXCore::Config::CONFIG_X86DISASSEMBLE,
                           traceEnabled ? "1" : "0");
      FexBlockTraceCount.store(0, std::memory_order_relaxed);
      LogMan::Msg::InstallHandler(FexMessageHandler);
    }
    ++Users;
    return true;
  }

  static EngineResult<bool> Release() {
    std::scoped_lock lock {Mutex};
    if (Users == 0) return Failure(EngineStage::Teardown, EINVAL);
    if (--Users == 0) {
      FEXCore::Config::Shutdown();
      LogMan::Msg::UnInstallHandler();
    }
    return true;
  }

private:
  static inline std::mutex Mutex;
  static inline size_t Users {};
};

class Mapping final {
public:
  Mapping(size_t size, int protection)
    : Size {size}
    , Address {mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}
    , LastError {Address == MAP_FAILED ? errno : 0} {}

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  ~Mapping() {
    if (Address != MAP_FAILED && munmap(Address, Size) != 0) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsValid() const {
    return Address != MAP_FAILED;
  }

  [[nodiscard]] int Error() const {
    return LastError;
  }

  [[nodiscard]] void* Get() const {
    return Address;
  }

  [[nodiscard]] EngineResult<bool> Protect(int protection) {
    if (mprotect(Address, Size, protection) != 0) {
      return Failure(EngineStage::Mapping, errno);
    }
    return true;
  }

  [[nodiscard]] EngineResult<bool> Release() {
    if (Address == MAP_FAILED) {
      return true;
    }
    if (munmap(Address, Size) != 0) {
      return Failure(EngineStage::Teardown, errno);
    }
    Address = MAP_FAILED;
    return true;
  }

private:
  size_t Size;
  void* Address;
  int LastError;
};

#if defined(__APPLE__) && TARGET_OS_IPHONE
// DualMappedMapping — iOS equivalent of Mapping for allocations that need PROT_EXEC.
//
// On iOS, an unentitled app cannot add PROT_EXEC to any page via mprotect. Instead
// we use BreakpointJIT.framework's BRK-trap protocol (serviced by StikDebug) to get
// a pair of virtual addresses aliasing the same physical pages: one writable (RW)
// for code-writing and one executable (RX) for CPU dispatch. See ios_jit_allocator.h.
//
// Call sites that formerly did:
//   Mapping m(page_size, PROT_READ | PROT_WRITE);
//   write_code(m.Get());  // write through RW addr
//   m.Protect(PROT_READ | PROT_EXEC);
//   execute(m.Get());     // execute same addr — FAILS on iOS
// now do:
//   DualMappedMapping m(page_size);
//   write_code(m.GetRW());   // write through RW addr
//   execute(m.GetRX());      // execute via distinct RX addr — OK on iOS
class DualMappedMapping final {
public:
  explicit DualMappedMapping(size_t size)
    : Region {Core::DualMappedRegion::Allocate(size)} {}

  DualMappedMapping(const DualMappedMapping&) = delete;
  DualMappedMapping& operator=(const DualMappedMapping&) = delete;

  [[nodiscard]] bool IsValid() const {
    return Region.IsValid();
  }

  // Returns ENODEV when StikDebug is not attached (BreakGetJITMapping returned nullptr).
  [[nodiscard]] int Error() const {
    return Region.IsValid() ? 0 : ENODEV;
  }

  // Write JIT code through this address.
  [[nodiscard]] void* GetRW() const {
    return Region.rw_addr;
  }

  // Execute JIT code through this address (distinct from RW on iOS).
  [[nodiscard]] void* GetRX() const {
    return Region.rx_addr;
  }

  // For callers that only need the exec address (e.g. registering a callback return
  // address with the signal delegator): returns the RX address.
  [[nodiscard]] void* Get() const {
    return GetRX();
  }

private:
  Core::DualMappedRegion Region;
};
#endif // defined(__APPLE__) && TARGET_OS_IPHONE

class CallRetStack final {
public:
  CallRetStack()
    : Address {mmap(nullptr, kAllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}
    , LastError {Address == MAP_FAILED ? errno : 0} {}

  CallRetStack(const CallRetStack&) = delete;
  CallRetStack& operator=(const CallRetStack&) = delete;

  ~CallRetStack() {
    if (Address != MAP_FAILED && munmap(Address, kAllocationSize) != 0) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsReserved() const {
    return Address != MAP_FAILED;
  }

  [[nodiscard]] int Error() const {
    return LastError;
  }

  [[nodiscard]] EngineResult<bool> MakeWritable() const {
    if (mprotect(StackBase(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE) != 0) {
      return Failure(EngineStage::Mapping, errno);
    }
    return true;
  }

  void Initialize(FEXCore::Core::InternalThreadState* thread) const {
    thread->CallRetStackBase = StackBase();
    // FEXCore's call-return prediction stack (Dispatcher.cpp: REG_CALLRET_SP) grows downward
    // from wherever callret_sp starts, so it needs to start near the *top* of the writable
    // region (StackBase() + CALLRET_STACK_SIZE) to have the full size available to grow into
    // before running into StackBase() itself (and the PROT_NONE guard page just before it).
    // This previously divided by 4, starting only 25% of the way in and leaving just a
    // quarter of CALLRET_STACK_SIZE (1MB, not 4MB) of actual headroom -- confirmed on-device
    // as an eventual out-of-bounds stp write once a long enough session accumulates more
    // nested call/return activity than that quarter could hold.
    //
    // Starting at EXACTLY StackBase() + CALLRET_STACK_SIZE is its own bug, though (confirmed
    // on-device: broke every single game immediately, not just long sessions). MakeWritable()
    // above only mprotects the CALLRET_STACK_SIZE bytes starting at StackBase() --
    // StackBase()+CALLRET_STACK_SIZE is the first byte of the *top* PROT_NONE guard page, one
    // past the last valid byte, not a valid address itself. The push side (Dispatcher.cpp)
    // pre-decrements before writing, so it alone would never touch that address -- but the
    // return/pop side does `ldr callret_sp` then `ldp TMP1, TMP2, [callret_sp]` (a plain
    // offset load, no pre-decrement) *before* incrementing afterward, so it reads directly
    // from whatever callret_sp currently is. The very first return this mechanism ever
    // resolves for a fresh thread reads from this exact starting value before any push has
    // ever decremented it -- landing squarely in the guard page. Back off by one prediction
    // slot (0x10, matching the stp/ldp width Dispatcher.cpp uses) so even that first,
    // un-decremented read stays inside the writable region.
    thread->CurrentFrame->State.callret_sp = reinterpret_cast<uint64_t>(StackBase()) +
      FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE - 0x10;
  }

private:
  static constexpr size_t kAllocationSize = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * kRequiredPageSize;

  [[nodiscard]] void* StackBase() const {
    return static_cast<uint8_t*>(Address) + kRequiredPageSize;
  }

  void* Address;
  int LastError;
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

class ThreadScope final {
public:
  ThreadScope(FEXCore::Context::Context& context, FEXCore::Core::InternalThreadState* thread)
    : Context {context}
    , Thread {thread} {}

  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;

  ~ThreadScope() {
    if (Thread != nullptr) {
      Context.DestroyThread(Thread);
    }
  }

private:
  FEXCore::Context::Context& Context;
  FEXCore::Core::InternalThreadState* Thread;
};

class BridgeSignalDelegator final : public FEXCore::SignalDelegator {
public:
  explicit BridgeSignalDelegator(uintptr_t callbackReturn)
    : CallbackReturn {callbackReturn} {}

  uintptr_t GetThunkCallbackRET() const override {
    return CallbackReturn;
  }

  void SetThunkCallbackRET(uintptr_t callbackReturn) {
    CallbackReturn = callbackReturn;
  }

private:
  uintptr_t CallbackReturn;
};

class CallbackReturnScope final {
public:
  CallbackReturnScope(BridgeSignalDelegator& delegator, uintptr_t callbackReturn)
    : Delegator {delegator}
    , Previous {delegator.GetThunkCallbackRET()} {
    Delegator.SetThunkCallbackRET(callbackReturn);
  }

  CallbackReturnScope(const CallbackReturnScope&) = delete;
  CallbackReturnScope& operator=(const CallbackReturnScope&) = delete;

  ~CallbackReturnScope() {
    Delegator.SetThunkCallbackRET(Previous);
  }

private:
  BridgeSignalDelegator& Delegator;
  uintptr_t Previous;
};

class BridgeSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  struct InvocationState final {
    std::optional<EngineFailure> Failure;
    uint64_t Result {};
    bool WasInvoked {};
    BridgeSyscallHandler* Owner {};
    FEXCore::Core::InternalThreadState* Thread {};

    void Reset() {
      Failure.reset();
      Result = 0;
      WasInvoked = false;
      Owner = nullptr;
      Thread = nullptr;
    }
  };

  class InvocationScope final {
  public:
    InvocationScope(BridgeSyscallHandler& owner, InvocationState& state,
                    FEXCore::Core::InternalThreadState* thread)
      : Previous {ActiveInvocation} {
      state.Reset();
      state.Owner = &owner;
      state.Thread = thread;
      ActiveInvocation = &state;
    }

    InvocationScope(const InvocationScope&) = delete;
    InvocationScope& operator=(const InvocationScope&) = delete;

    ~InvocationScope() {
      ActiveInvocation = Previous;
    }

  private:
    InvocationState* Previous;
  };

  explicit BridgeSyscallHandler(GuestBridge& bridge)
    : Bridge {bridge} {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }

  // Run deferred Orbis exception handler (Unity SIGUSR1 / signal 30) via nested
  // HandleCallback. Mirrors desktop Windows APC ExceptionHandler: guest handler
  // must actually run so GC STW can complete. Never rewrite host RIP/RSP.
  void FlushPendingOrbisSignal() {
    if (!PendingOrbisSignal.Pending || PendingOrbisSignal.Flushing ||
        ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr ||
        PendingOrbisSignal.Handler == 0) {
      return;
    }
    const auto handler = PendingOrbisSignal.Handler;
    const auto sig = PendingOrbisSignal.OrbisSig;
    PendingOrbisSignal.Pending = false;
    PendingOrbisSignal.Handler = 0;
    PendingOrbisSignal.Flushing = true;

    auto* thread = ActiveFexExecution.Thread;
    auto* ctx = ActiveFexExecution.Context;
    auto& state = thread->CurrentFrame->State;
    using namespace FEXCore::X86State;

    // Nested invocation so handler HLE does not clobber the outer syscall frame.
    InvocationState nested_invocation;
    InvocationScope nested_scope {*this, nested_invocation, thread};

    // Guest stack: prefer CurrentFrame RSP when it looks like PS4 VA; otherwise
    // fall back to this pthread's guest stack (first GC kill often arrives before
    // FEX has spilled guest RSP into CurrentFrame — frame holds host values).
    const auto looks_guest = [](uint64_t a) {
      // Tight PS4 user window. Host Android stacks sit at ~0x7e.. / 0x7c.. and
      // must not be used as guest RSP (HandleCallback + x86 stack ops crash).
      return a >= 0x100000000ULL && a < 0x900000000ULL;
    };
    uint64_t work_rsp = state.gregs[REG_RSP];
    if (!looks_guest(work_rsp)) {
      // Prefer Orbis pthread guest stack (strong FexCurrentGuestStackTop in
      // exception.cpp). Harness keeps the weak stub → 0.
      const auto top = Libraries::Kernel::FexCurrentGuestStackTop();
      if (top != 0 && looks_guest(top - 0x100)) {
        work_rsp = top - 0x100;
        std::fprintf(stderr,
                     "BACHATA_FEX_SIGNAL use pthread stack top=%#lx "
                     "(frame_rsp was %#lx)\n",
                     static_cast<unsigned long>(work_rsp),
                     static_cast<unsigned long>(state.gregs[REG_RSP]));
      }
    }

    // Save interrupted frame (may hold host values mid-HLE).
    std::array<uint64_t, 16> saved_gprs {};
    std::copy(std::begin(state.gregs), std::begin(state.gregs) + 16, saved_gprs.begin());
    const uint64_t saved_rip = state.rip;
    const uint64_t saved_rsp = state.gregs[REG_RSP];
    if (!looks_guest(work_rsp)) {
      std::fprintf(stderr,
                   "BACHATA_FEX_SIGNAL flush abort: no guest stack frame_rsp=%#lx\n",
                   static_cast<unsigned long>(saved_rsp));
      PendingOrbisSignal.Pending = true;
      PendingOrbisSignal.Handler = handler;
      PendingOrbisSignal.OrbisSig = sig;
      PendingOrbisSignal.Flushing = false;
      return;
    }

    // Zero guest GPRs for soft handler: frame may contain host SRA garbage that
    // looks like pointers (null+0xf8 SEGV). Keep FS/GS as-is for TLS.
    for (size_t i = 0; i < 16; ++i) {
      state.gregs[i] = 0;
    }
    state.gregs[REG_RSP] = work_rsp;

    // PS4Util soft-handler pattern (Galak-Z):
    //   lea rax, [counter]; inc [rax]
    //   cmp edi, 30; jne fallback
    //   mov rdx, [rsi+0xf8]   // uctx->uc_mcontext.mc_rsp  (offset 0xf8)
    //   mov rdi, rsi          // uctx
    //   mov rcx, [handler_slot]; mov rsi, rdx; jmp rcx
    // So second arg to real Unity handler is mc_rsp — MUST be valid guest SP.
    uint64_t ctx_addr = 0;
    constexpr uint64_t kUcontextBytes =
        (sizeof(Libraries::Kernel::Ucontext) + 0x3full) & ~uint64_t{0x3f};
    if (work_rsp > kUcontextBytes + 0x100) {
      ctx_addr = (work_rsp - kUcontextBytes) & ~uint64_t{0xf};
      auto* uctx = reinterpret_cast<Libraries::Kernel::Ucontext*>(
          static_cast<uintptr_t>(ctx_addr));
      std::memset(uctx, 0, sizeof(*uctx));
      auto& mc = uctx->uc_mcontext;
      // Stack for the real handler (read at uctx+0xf8).
      mc.mc_rsp = work_rsp;
      mc.mc_rbp = work_rsp;
      if (looks_guest(saved_rip)) {
        mc.mc_rip = saved_rip;
      } else {
        // Dummy guest RIP in eboot range so stack-walkers see a code address.
        mc.mc_rip = 0x8000000a0ULL;
      }
      mc.mc_fsbase = state.fs_cached;
      mc.mc_gsbase = state.gs_cached;
      mc.mc_fs = static_cast<u16>(state.fs_cached & 0xffff);
      mc.mc_gs = static_cast<u16>(state.gs_cached & 0xffff);
      // Handler stack below the uctx blob.
      state.gregs[REG_RSP] = ctx_addr;
    }
    state.gregs[REG_RDI] = static_cast<uint64_t>(static_cast<uint32_t>(sig));
    state.gregs[REG_RSI] = ctx_addr;

    std::fprintf(stderr,
                 "BACHATA_FEX_SIGNAL flush orbis_sig=%d handler=%#lx ctx=%#lx "
                 "work_rsp=%#lx fs=%#lx\n",
                 sig, static_cast<unsigned long>(handler),
                 static_cast<unsigned long>(ctx_addr),
                 static_cast<unsigned long>(work_rsp),
                 static_cast<unsigned long>(state.fs_cached));
    // Dump first 16 bytes of guest handler + probe QueryExecutableRange.
    {
      const auto range = QueryGuestExecutableRange(thread, handler);
      std::fprintf(stderr,
                   "BACHATA_FEX_SIGNAL handler_range begin=%#lx size=%#lx writable=%d\n",
                   static_cast<unsigned long>(range.Base),
                   static_cast<unsigned long>(range.Size),
                   range.Writable ? 1 : 0);
      const auto* bytes = reinterpret_cast<const unsigned char*>(
          static_cast<uintptr_t>(handler));
      std::fprintf(stderr, "BACHATA_FEX_SIGNAL handler_bytes");
      for (int i = 0; i < 64; ++i) {
        std::fprintf(stderr, " %02x", bytes[i]);
      }
      std::fprintf(stderr, "\n");
      // Global the first LEA likely targets (rip+7+disp32 at +3).
      if (bytes[0] == 0x48 && bytes[1] == 0x8d && bytes[2] == 0x05) {
        const auto disp = static_cast<int32_t>(bytes[3] | (bytes[4] << 8) |
                                               (bytes[5] << 16) | (bytes[6] << 24));
        const auto target = handler + 7 + static_cast<std::uintptr_t>(disp);
        const auto* g = reinterpret_cast<const unsigned char*>(
            static_cast<uintptr_t>(target));
        std::fprintf(stderr,
                     "BACHATA_FEX_SIGNAL lea_target=%#lx qwords=%#lx %#lx %#lx %#lx\n",
                     static_cast<unsigned long>(target),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 8)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 16)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 24)));
        // Real Unity handler slot is typically at lea_target+0x10 (third qword).
        const auto real_fn = *reinterpret_cast<const uint64_t*>(g + 16);
        if (real_fn != 0) {
          const auto* rb = reinterpret_cast<const unsigned char*>(
              static_cast<uintptr_t>(real_fn));
          std::fprintf(stderr, "BACHATA_FEX_SIGNAL real_fn=%#lx bytes",
                       static_cast<unsigned long>(real_fn));
          for (int i = 0; i < 32; ++i) {
            std::fprintf(stderr, " %02x", rb[i]);
          }
          std::fprintf(stderr, "\n");
          // Verify uctx+0xf8 == mc_rsp we wrote.
          if (ctx_addr != 0) {
            const auto at_f8 = *reinterpret_cast<const uint64_t*>(
                static_cast<uintptr_t>(ctx_addr + 0xf8));
            std::fprintf(stderr,
                         "BACHATA_FEX_SIGNAL uctx+0xf8(mc_rsp)=%#lx work_rsp=%#lx\n",
                         static_cast<unsigned long>(at_f8),
                         static_cast<unsigned long>(work_rsp));
          }
        }
      }
    }
    std::fprintf(stderr,
                 "BACHATA_FEX_SIGNAL Exception raised successfully orbis_sig=%d handler=%#lx\n",
                 sig, static_cast<unsigned long>(handler));
    std::fprintf(stderr, "BACHATA_FEX_SIGNAL callback_ret=%#lx\n",
                 static_cast<unsigned long>(thread->CurrentFrame->Pointers.ThunkCallbackRet));

    ctx->HandleCallback(thread, handler);

    std::fprintf(stderr, "BACHATA_FEX_SIGNAL handler returned rip=%#lx rsp=%#lx\n",
                 static_cast<unsigned long>(state.rip),
                 static_cast<unsigned long>(state.gregs[REG_RSP]));

    // Restore interrupted frame for resume (HLE or JIT trampoline).
    std::copy(saved_gprs.begin(), saved_gprs.end(), std::begin(state.gregs));
    state.rip = saved_rip;
    state.gregs[REG_RSP] = saved_rsp;
    PendingOrbisSignal.Flushing = false;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments*) override {
    // Desktop APC ExceptionHandler runs guest handler on the target thread around
    // normal execution. On FEX, host pthread_kill only wakes + queues Pending;
    // deliver here at HLE boundary (entry) so GC STW sees the handler.
    g_threads_in_hle_syscall.fetch_add(1, std::memory_order_acq_rel);
    FlushPendingOrbisSignal();
    auto* invocation = ActiveInvocation;
    if (invocation == nullptr) {
      g_threads_in_hle_syscall.fetch_sub(1, std::memory_order_acq_rel);
      return static_cast<uint64_t>(-EPERM);
    }
    if (frame == nullptr) {
      invocation->Failure = Failure(EngineStage::Bridge, EFAULT);
      g_threads_in_hle_syscall.fetch_sub(1, std::memory_order_acq_rel);
      return static_cast<uint64_t>(-EFAULT);
    }

    Core::GuestCpu::HleCallFrame hleFrame{};
    hleFrame.operation = frame->State.gregs[FEXCore::X86State::REG_RAX];
    std::copy(std::begin(frame->State.gregs), std::end(frame->State.gregs), hleFrame.gpr.begin());
    hleFrame.gpr[FEXCore::X86State::REG_RCX] = frame->State.gregs[FEXCore::X86State::REG_R10];
    hleFrame.rsp = frame->State.gregs[FEXCore::X86State::REG_RSP];
    for (size_t index = 0; index < hleFrame.xmm.size(); ++index) {
      hleFrame.xmm[index] = {frame->State.xmm.sse.data[index][0], frame->State.xmm.sse.data[index][1]};
    }
    const bool is_main = IsCurrentThreadGameMain();
    const bool is_rt = !is_main && IsCurrentThreadRenderingThread();
    if (is_main || is_rt) {
      EnsureHleStallWatchdogStarted();
      auto& op_slot = is_main ? g_last_hle_operation : g_last_hle_operation_rt;
      auto& start_slot = is_main ? g_last_hle_operation_start_ms : g_last_hle_operation_start_ms_rt;
      op_slot.store(hleFrame.operation, std::memory_order_relaxed);
      start_slot.store(Common::BootElapsedMs(), std::memory_order_relaxed);
    }
    auto result = Bridge.Invoke(hleFrame);
    // Only meaningful while this exact call is still in flight; once it returns (here, on
    // every path below) the watchdog should stop attributing any new stall to it. Not reset to
    // 0 unconditionally by the *next* call's own store above only because that store might be
    // arbitrarily far in the future if this thread goes on to block in a signal handler or
    // elsewhere before making another HLE call -- clearing it here, right as this call
    // completes, is what actually matters.
    if (is_main || is_rt) {
      auto& start_slot = is_main ? g_last_hle_operation_start_ms : g_last_hle_operation_start_ms_rt;
      start_slot.store(0, std::memory_order_relaxed);
    }
    if (const auto* error = std::get_if<EngineFailure>(&result)) {
      invocation->Failure = *error;
      frame->State.gregs[FEXCore::X86State::REG_RAX] = static_cast<uint64_t>(-error->Error);
      // Still try to deliver if kill arrived while blocked in host HLE.
      FlushPendingOrbisSignal();
      g_threads_in_hle_syscall.fetch_sub(1, std::memory_order_acq_rel);
      return frame->State.gregs[FEXCore::X86State::REG_RAX];
    }

    std::copy(hleFrame.gpr.begin(), hleFrame.gpr.end(), std::begin(frame->State.gregs));
    for (size_t index = 0; index < hleFrame.xmm.size(); ++index) {
      frame->State.xmm.sse.data[index][0] = hleFrame.xmm[index][0];
      frame->State.xmm.sse.data[index][1] = hleFrame.xmm[index][1];
    }
    invocation->Result = frame->State.gregs[FEXCore::X86State::REG_RAX];
    invocation->WasInvoked = true;
    // Kill often lands while target is blocked inside host futex/HLE. Flush after
    // Invoke so handler runs before returning to pure guest JIT.
    FlushPendingOrbisSignal();
    g_threads_in_hle_syscall.fetch_sub(1, std::memory_order_acq_rel);
    return invocation->Result;
  }

  EngineResult<bool> RegisterThread(FEXCore::Core::InternalThreadState* thread,
                                    const std::vector<Core::GuestExecutionRange>& ranges) {
    if (thread == nullptr) return Failure(EngineStage::Thread, EINVAL);
    std::scoped_lock lock {RangesMutex};
    const auto [_, inserted] = ExecutableRanges.emplace(thread, ranges);
    return inserted ? EngineResult<bool> {true} : EngineResult<bool> {Failure(EngineStage::Thread, EEXIST)};
  }

  void UnregisterThread(FEXCore::Core::InternalThreadState* thread) {
    std::scoped_lock lock {RangesMutex};
    ExecutableRanges.erase(thread);
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* thread,
                                                               uint64_t address) override {
    {
      std::scoped_lock lock {RangesMutex};
      const auto ranges = ExecutableRanges.find(thread);
      if (ranges != ExecutableRanges.end()) {
        for (const auto& range : ranges->second) {
          if (range.Executable && Contains(range, static_cast<std::uintptr_t>(address), 1)) {
            return {range.Begin, range.Size, range.Writable};
          }
        }
      }
    }
    const auto dynamicRange = Bridge.QueryExecutableRange(static_cast<std::uintptr_t>(address));
    if (dynamicRange && dynamicRange->Executable && Contains(*dynamicRange, address, 1)) {
      return {dynamicRange->Begin, dynamicRange->Size, dynamicRange->Writable};
    }
    return {};
  }

  FEXCore::Core::InternalThreadState* ActiveThread() const {
    if (ActiveInvocation == nullptr || ActiveInvocation->Owner != this) return nullptr;
    return ActiveInvocation->Thread;
  }

  const std::optional<EngineFailure>& ActiveFailure() const {
    static const std::optional<EngineFailure> noFailure;
    if (ActiveInvocation == nullptr || ActiveInvocation->Owner != this) return noFailure;
    return ActiveInvocation->Failure;
  }

  bool IsWritableRange(FEXCore::Core::InternalThreadState* thread, std::uintptr_t address,
                       std::size_t size) {
    std::scoped_lock lock {RangesMutex};
    const auto ranges = ExecutableRanges.find(thread);
    if (ranges == ExecutableRanges.end()) return false;
    return std::ranges::any_of(ranges->second, [&](const Core::GuestExecutionRange& range) {
      return range.Writable && Contains(range, address, size);
    });
  }

  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  [[nodiscard]] const std::optional<EngineFailure>& FailureResult(const InvocationState& state) const {
    return state.Failure;
  }

  [[nodiscard]] uint64_t Result(const InvocationState& state) const {
    return state.Result;
  }

  [[nodiscard]] bool Invoked(const InvocationState& state) const {
    return state.WasInvoked;
  }

private:
  static thread_local InvocationState* ActiveInvocation;

  GuestBridge& Bridge;
  std::mutex RangesMutex;
  std::unordered_map<FEXCore::Core::InternalThreadState*, std::vector<Core::GuestExecutionRange>> ExecutableRanges;
};

thread_local BridgeSyscallHandler::InvocationState* BridgeSyscallHandler::ActiveInvocation {};

class ExecutableRangeScope final {
public:
  ExecutableRangeScope(BridgeSyscallHandler& handler, FEXCore::Core::InternalThreadState* thread)
    : Handler {handler}
    , Thread {thread} {}

  ExecutableRangeScope(const ExecutableRangeScope&) = delete;
  ExecutableRangeScope& operator=(const ExecutableRangeScope&) = delete;

  ~ExecutableRangeScope() {
    Handler.UnregisterThread(Thread);
  }

private:
  BridgeSyscallHandler& Handler;
  FEXCore::Core::InternalThreadState* Thread;
};

void AppendImmediate(std::vector<uint8_t>& code, uint64_t value) {
  const auto offset = code.size();
  code.resize(offset + sizeof(value));
  std::memcpy(code.data() + offset, &value, sizeof(value));
}

uint64_t DoubleBits(double value) {
  uint64_t bits {};
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

struct GuestCode final {
  std::vector<uint8_t> Bytes;
  size_t CallbackOffset {};
  size_t CallbackReturnOffset {};
  size_t InvalidationOffset {};
  size_t InvalidationImmediateOffset {};
  size_t ThreadOffset {};
};

GuestCode BuildGuestCode() {
  constexpr uint64_t bridgeOperation = 0xB4C4'F001ULL;
  constexpr uint64_t bridgeArgument = 0x1020'3040'5060'7080ULL;

  GuestCode result;
  auto& code = result.Bytes;
  code.insert(code.end(), {0x48, 0xb8}); // mov rax, kAddLeft
  AppendImmediate(code, kAddLeft);
  code.insert(code.end(), {0x48, 0xbb}); // mov rbx, kAddRight
  AppendImmediate(code, kAddRight);
  code.insert(code.end(), {0x48, 0x01, 0xd8}); // add rax, rbx
  code.insert(code.end(), {0x49, 0x89, 0xc0}); // mov r8, rax
  code.insert(code.end(), {0x48, 0xb9}); // mov rcx, kXorLeft
  AppendImmediate(code, kXorLeft);
  code.insert(code.end(), {0x48, 0xba}); // mov rdx, kXorRight
  AppendImmediate(code, kXorRight);
  code.insert(code.end(), {0x48, 0x31, 0xd1}); // xor rcx, rdx
  code.insert(code.end(), {0x49, 0x89, 0xc9}); // mov r9, rcx
  code.insert(code.end(), {0xf2, 0x0f, 0x58, 0xc1}); // addsd xmm0, xmm1

  code.insert(code.end(), {0x48, 0xb8}); // mov rax, bridge operation
  AppendImmediate(code, bridgeOperation);
  code.insert(code.end(), {0x48, 0xbf}); // mov rdi, bridge argument
  AppendImmediate(code, bridgeArgument);
  code.insert(code.end(), {0x0f, 0x05}); // syscall
  code.insert(code.end(), {0x4d, 0x8b, 0x2c, 0x24}); // mov r13, [r12]

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

} // namespace

int BachataGetHgsCheckpoint() noexcept {
  return g_hgs_checkpoint.load(std::memory_order_relaxed);
}

bool BachataQueryGuestRipSyscall(uint64_t* out_rip, uint64_t* out_syscall) noexcept {
  // Async-signal-safe: only dereferences thread-local pointer + frame struct.
  // ActiveFexExecution is thread_local, set by FexExecutionSignalScope during
  // guest execution. If no FEX thread is active (e.g. crash in host-only init),
  // Context is null and we report unavailable.
  const auto& exec = ActiveFexExecution;
  if (exec.Context == nullptr || exec.Thread == nullptr || exec.Thread->CurrentFrame == nullptr) {
    return false;
  }
  // CurrentFrame->State holds the spilled guest GPRs at the last HLE/JIT boundary.
  // During mid-JIT execution these may be stale (live in SRA host regs), but for a
  // SIGSYS delivered at a host syscall boundary this is the best available view.
  const auto& state = exec.Thread->CurrentFrame->State;
  if (out_rip != nullptr) {
    *out_rip = state.rip;
  }
  if (out_syscall != nullptr) {
    *out_syscall = state.gregs[FEXCore::X86State::REG_RAX];
  }
  return true;
}

bool BachataDumpGuestRegisters(char* out_buf, std::size_t out_buf_size, uint64_t* out_rsp,
                                uint64_t* out_rbp) noexcept {
  if (out_buf == nullptr || out_buf_size == 0) {
    return false;
  }
  const auto& exec = ActiveFexExecution;
  if (exec.Context == nullptr || exec.Thread == nullptr || exec.Thread->CurrentFrame == nullptr) {
    return false;
  }
  const auto& gregs = exec.Thread->CurrentFrame->State.gregs;
  using namespace FEXCore::X86State;
  if (out_rsp != nullptr) {
    *out_rsp = gregs[REG_RSP];
  }
  if (out_rbp != nullptr) {
    *out_rbp = gregs[REG_RBP];
  }
  std::snprintf(
      out_buf, out_buf_size,
      "rax=%#llx rcx=%#llx rdx=%#llx rbx=%#llx rsp=%#llx rbp=%#llx rsi=%#llx rdi=%#llx "
      "r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx",
      static_cast<unsigned long long>(gregs[REG_RAX]), static_cast<unsigned long long>(gregs[REG_RCX]),
      static_cast<unsigned long long>(gregs[REG_RDX]), static_cast<unsigned long long>(gregs[REG_RBX]),
      static_cast<unsigned long long>(gregs[REG_RSP]), static_cast<unsigned long long>(gregs[REG_RBP]),
      static_cast<unsigned long long>(gregs[REG_RSI]), static_cast<unsigned long long>(gregs[REG_RDI]),
      static_cast<unsigned long long>(gregs[REG_R8]), static_cast<unsigned long long>(gregs[REG_R9]),
      static_cast<unsigned long long>(gregs[REG_R10]), static_cast<unsigned long long>(gregs[REG_R11]),
      static_cast<unsigned long long>(gregs[REG_R12]), static_cast<unsigned long long>(gregs[REG_R13]),
      static_cast<unsigned long long>(gregs[REG_R14]), static_cast<unsigned long long>(gregs[REG_R15]));
  return true;
}

// Diagnostic storage for a single, known-deterministic guest block (see Core.cpp's
// ContextImpl::CompileBlock, which populates this at compile time) and a crash-time comparison
// against it. Directly answers the one question neither the compile-time snapshot log nor the
// crash-time disassembly can answer alone: are the two reads of the *same* memory actually
// identical, or did something rewrite it in between? A plain global, not thread_local -- this
// is deliberately a single, process-wide slot for one specific address, not a general
// mechanism, matching the same "one known thing, own dedicated slot" reasoning as
// g_last_hle_operation_rt's comment above.
// 4096: matches Core.cpp's own CaptureLen cap (CompiledCode.Size, capped defensively) -- large
// enough to comfortably cover any real compiled block's full length, so the fault PC (which
// must fall somewhere inside this exact compile, since it shares the same guest RIP) is always
// within the captured range, not just the block's first few bytes.
constexpr int kKnownBlockSnapshotCap = 4096;
std::atomic<uintptr_t> g_known_block_code_ptr {0};
std::atomic<int> g_known_block_compile_count {0};
std::atomic<std::size_t> g_known_block_snapshot_len {0};
unsigned char g_known_block_snapshot[kKnownBlockSnapshotCap] = {};
std::atomic<uint64_t> g_known_block_compile_thread {0};

void BachataRecordKnownBlockSnapshot(uintptr_t writable_code_ptr, const unsigned char* bytes, std::size_t len) noexcept {
  len = std::min<std::size_t>(len, kKnownBlockSnapshotCap);
  g_known_block_compile_count.fetch_add(1, std::memory_order_relaxed);
  g_known_block_code_ptr.store(writable_code_ptr, std::memory_order_release);
  std::memcpy(g_known_block_snapshot, bytes, len);
  g_known_block_snapshot_len.store(len, std::memory_order_release);
  g_known_block_compile_thread.store(reinterpret_cast<uint64_t>(pthread_self()), std::memory_order_relaxed);
}

// Called from the crash path (signals.cpp) once a fault is confirmed at or inside the known
// block's recorded address. Re-reads the *current* bytes at that same address and compares them
// byte-for-byte against what was recorded at compile time -- IDENTICAL proves the compiled code
// itself was never wrong and something else (execution flow landing here incorrectly, or a
// register going stale some other way) is the real cause; any mismatch, reported with the exact
// offset and both values, proves something rewrote this memory after compilation, which then
// narrows the search to whatever else touches code memory (invalidation, a second compile of
// the same or an overlapping range, etc).
bool BachataCompareKnownBlockSnapshot(char* out_buf, std::size_t out_buf_size) noexcept {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (out_buf == nullptr || out_buf_size == 0) {
    return false;
  }
  const auto code_ptr = g_known_block_code_ptr.load(std::memory_order_acquire);
  const auto compile_count = g_known_block_compile_count.load(std::memory_order_relaxed);
  const auto snapshot_len = g_known_block_snapshot_len.load(std::memory_order_acquire);
  if (code_ptr == 0 || snapshot_len == 0) {
    std::snprintf(out_buf, out_buf_size, "no snapshot recorded (this block was never compiled this session)");
    return true;
  }
  const auto* current = reinterpret_cast<const volatile unsigned char*>(code_ptr);
  long first_mismatch = -1;
  unsigned char expected = 0;
  unsigned char actual = 0;
  for (std::size_t i = 0; i < snapshot_len; ++i) {
    const unsigned char cur = current[i];
    if (cur != g_known_block_snapshot[i]) {
      first_mismatch = static_cast<long>(i);
      expected = g_known_block_snapshot[i];
      actual = cur;
      break;
    }
  }
  if (first_mismatch < 0) {
    std::snprintf(out_buf, out_buf_size,
                  "IDENTICAL to compile-time snapshot across all %zu captured bytes (compiled %d "
                  "time(s), compile_thread=%#llx) -- the compiled bytes were never wrong, something "
                  "else explains the fault",
                  snapshot_len, compile_count,
                  static_cast<unsigned long long>(g_known_block_compile_thread.load(std::memory_order_relaxed)));
  } else {
    std::snprintf(out_buf, out_buf_size,
                  "MISMATCH at offset %ld (of %zu captured bytes): compile-time=%#04x current=%#04x "
                  "(compiled %d time(s), compile_thread=%#llx) -- this memory was rewritten after "
                  "compilation",
                  first_mismatch, snapshot_len, expected, actual, compile_count,
                  static_cast<unsigned long long>(g_known_block_compile_thread.load(std::memory_order_relaxed)));
  }
  return true;
#else
  static_cast<void>(out_buf);
  static_cast<void>(out_buf_size);
  return false;
#endif
}

bool BachataDumpHostCodeWords(void* fault_pc, char* out_buf, std::size_t out_buf_size) noexcept {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (fault_pc == nullptr || out_buf == nullptr || out_buf_size == 0) {
    return false;
  }
  // The guest_rip-based instruction dump (BachataQueryGuestRipSyscall) turned out to be a
  // stale JIT/HLE checkpoint, not the live fault site -- this dumps the ARM64 words actually
  // executing at the host fault PC instead, which is exact. Same writable-alias translation
  // HandleGuestSignal uses for its backpatch (fault_pc is the execute-only side of iOS's
  // dual JIT mapping, not directly readable); no ScopedJITWriteProtect needed since this
  // only reads, and elsewhere in FEXCore (JIT.cpp) GetWritableAddress is called bare for
  // reads too.
  const auto writable_pc = reinterpret_cast<uintptr_t>(FEXCore::Allocator::GetWritableAddress(fault_pc));
  if (writable_pc == 0) {
    return false;
  }
  // 24 words (4 bytes each) before fault_pc through 24 after: ARM64 instructions are fixed
  // 4-byte width, so this is exact context, not a guess at instruction boundaries the way
  // the earlier variable-length x86 byte dump was. Widened from an original 8/8 while chasing
  // a reproducible "write to 0x0" crash whose immediate 8-word-before window turned out to
  // span a suspiciously round (16KB-aligned) boundary -- more context on both sides helps tell
  // whether that's this block's own start or a neighboring, unrelated block.
  constexpr int kWordsBefore = 24;
  constexpr int kWordsAfter = 24;
  char* w = out_buf;
  char* const end = out_buf + out_buf_size;
  for (int i = -kWordsBefore; i <= kWordsAfter && w < end; ++i) {
    const auto* word_ptr =
        reinterpret_cast<const volatile uint32_t*>(writable_pc + static_cast<ptrdiff_t>(i) * 4);
    const uint32_t word = *word_ptr;
    const int written = std::snprintf(w, static_cast<std::size_t>(end - w), "%s%08x", i == 0 ? "[" : "", word);
    if (written <= 0) break;
    w += written;
    if (i == 0 && w < end) {
      w += std::snprintf(w, static_cast<std::size_t>(end - w), "]");
    }
    if (w < end) {
      w += std::snprintf(w, static_cast<std::size_t>(end - w), " ");
    }
  }
  return true;
#else
  static_cast<void>(fault_pc);
  if (out_buf != nullptr && out_buf_size > 0) {
    out_buf[0] = '\0';
  }
  return false;
#endif
}

bool BachataReconstructAccurateGuestRIP(void* fault_pc, uint64_t* out_rip) noexcept {
  // BachataQueryGuestRipSyscall only reports the last JIT/HLE checkpoint's Frame->State.rip --
  // accurate at a syscall boundary, but stale mid-block, since FEX doesn't update it on every
  // instruction while executing straight-line JIT code. For a fault that lands deep inside a
  // large, branch-free block (the exact shape of the block chased for the Rocket League
  // null-write crash: a single ~25KB compiled fragment with no internal branches at all), that
  // stale RIP is just the block's entry point, not the actual faulting instruction -- nowhere
  // near enough to identify which of hundreds of near-identical RIP-relative stores in the block
  // is the one that goes wrong.
  //
  // FEXCore::Context::Context::RestoreRIPFromHostPC exists specifically to solve this: every
  // compiled block carries a side-table (JITCodeTail's RIP entries, see JIT.cpp's emission of
  // DebugData->GuestOpcodes) mapping host PC ranges back to exact guest RIPs, built for this
  // exact purpose. It was never called anywhere in this codebase before now -- Core.cpp's
  // GetFrameBlockInfo dereferenced the execute-only alias of InlineJITBlockHeader directly,
  // which is the same iOS dual-mapped-JIT trap Arm64.cpp's SIGBUS handler already hit and fixed
  // once (undeliverable second fault, silently kills the handling thread, zero further output).
  // Fixed at the source (Core.cpp) rather than worked around here, since every caller of
  // RestoreRIPFromHostPC benefits, not just this one diagnostic.
  if (fault_pc == nullptr || out_rip == nullptr) {
    return false;
  }
  const auto& exec = ActiveFexExecution;
  if (exec.Context == nullptr || exec.Thread == nullptr || exec.Thread->CurrentFrame == nullptr) {
    return false;
  }
  *out_rip = exec.Context->RestoreRIPFromHostPC(exec.Thread, reinterpret_cast<uint64_t>(fault_pc));
  return true;
}

bool BachataDescribeHostFaultAddress(void* fault_addr, char* out_buf, std::size_t out_buf_size) noexcept {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (out_buf == nullptr || out_buf_size == 0) {
    return false;
  }
  uintptr_t region_base = 0;
  std::size_t offset = 0;
  std::size_t size = 0;
  const auto kind = FEXCore::Allocator::iOSJITDescribeAddress(fault_addr, &region_base, &offset, &size);
  const char* kind_str = "not tracked (not inside any currently-live JIT allocation)";
  if (kind == FEXCore::Allocator::iOSJITAddressKind::LiveAllocation) {
    kind_str = "inside a currently-live JIT allocation (execute grant likely never landed, or "
              "was lost since -- see the reprepare attempt below)";
  }
  if (kind == FEXCore::Allocator::iOSJITAddressKind::NotTracked) {
    std::snprintf(out_buf, out_buf_size, "%s", kind_str);
  } else {
    std::snprintf(out_buf, out_buf_size, "%s (region base=%#lx, offset=%zu, size=%zu)", kind_str,
                  static_cast<unsigned long>(region_base), offset, size);
  }
  return true;
#else
  static_cast<void>(fault_addr);
  static_cast<void>(out_buf);
  static_cast<void>(out_buf_size);
  return false;
#endif
}

bool BachataDumpDispatcherState(char* out_buf, std::size_t out_buf_size) noexcept {
  const auto& exec = ActiveFexExecution;
  if (exec.Context == nullptr || exec.Thread == nullptr || out_buf == nullptr || out_buf_size == 0) {
    return false;
  }
  return exec.Context->DumpDispatcherStateForDiagnostics(exec.Thread, out_buf, out_buf_size);
}

void FlushPendingGuestOrbisSignal() noexcept {
  // Called from blocking HLE waits (semaphore/cond) to deliver a queued Orbis
  // guest signal at a safe point on the parked guest thread. Runs the guest
  // handler via nested HandleCallback (never from async-signal context). No-op
  // when nothing is pending or no FEX guest thread is active on this host thread.
  if (ActiveFexExecution.Syscalls != nullptr) {
    ActiveFexExecution.Syscalls->FlushPendingOrbisSignal();
  }
}

bool DeliverGuestOrbisSignal(int orbis_sig, siginfo_t* info, void* rawContext,
                             std::uintptr_t guest_handler) noexcept {
  static_cast<void>(info);
  if (guest_handler == 0) {
    return false;
  }

  // Queue for this thread (SigactionHandler runs on the kill target). It is not
  // safe to run the guest handler from async-signal context (nested
  // HandleCallback re-enters the JIT), so only set Pending here. Delivery happens
  // at the next safe HLE boundary: HandleSyscall for running JIT threads, or the
  // blocking semaphore/cond waits (FlushPendingGuestOrbisSignal) for threads
  // parked in a host futex, e.g. the GC Finalizer during stop-the-world.
  PendingOrbisSignal.Handler = guest_handler;
  PendingOrbisSignal.OrbisSig = orbis_sig;
  PendingOrbisSignal.Pending = true;
  PendingOrbisSignal.HasHostSnapshot = false;
  const bool active = ActiveFexExecution.Thread != nullptr &&
                      ActiveFexExecution.Syscalls != nullptr;

  std::uint64_t host_pc = 0;
  std::uint64_t host_sp = 0;
  bool in_jit = false;

#if defined(__aarch64__)
  if (rawContext != nullptr) {
    const auto* context = reinterpret_cast<const ucontext_t*>(rawContext);
#ifdef __APPLE__
    const auto& ts = context->uc_mcontext->__ss;
    host_pc = static_cast<std::uint64_t>(arm_thread_state64_get_pc(ts));
    host_sp = static_cast<std::uint64_t>(arm_thread_state64_get_sp(ts));
#else
    host_pc = static_cast<std::uint64_t>(context->uc_mcontext.pc);
    host_sp = static_cast<std::uint64_t>(context->uc_mcontext.sp);
#endif
    if (ActiveFexExecution.Context != nullptr && ActiveFexExecution.Thread != nullptr) {
      in_jit = ActiveFexExecution.Context->IsAddressInCodeBuffer(
          ActiveFexExecution.Thread, static_cast<std::uintptr_t>(host_pc));
    }
  }
#else
  static_cast<void>(rawContext);
#endif

  std::fprintf(stderr,
               "BACHATA_FEX_SIGNAL defer orbis_sig=%d handler=%#lx active=%d host_pc=%#lx host_sp=%#lx in_jit=%d\n",
               orbis_sig, static_cast<unsigned long>(guest_handler), active ? 1 : 0,
               static_cast<unsigned long>(host_pc), static_cast<unsigned long>(host_sp),
               in_jit ? 1 : 0);

  // Delivery is deferred to a safe HLE boundary. It is never safe to run the
  // guest handler (nested HandleCallback re-enters the JIT) from async-signal
  // context, and diverting the host PC to resume a blocked libc futex proved
  // unrecoverable (nested signal reentrancy → SIGILL). Instead the queued
  // handler runs from HandleSyscall (running JIT threads) or from the blocking
  // semaphore/cond waits via FlushPendingGuestOrbisSignal (threads parked in a
  // host futex, e.g. the GC Finalizer during stop-the-world).
  return true;
}


namespace {
// Writes `value` in the given base (10 or 16) into *out, advancing it. No allocation,
// no libc formatting calls -- unlike vsnprintf, this is genuinely safe to call from a
// signal handler.
void SignalSafeWriteUnsigned(char*& out, char* end, unsigned long long value, int base) {
  char digits[32];
  int n = 0;
  if (value == 0) {
    digits[n++] = '0';
  } else {
    while (value != 0 && n < static_cast<int>(sizeof(digits))) {
      const unsigned long long digit = value % static_cast<unsigned long long>(base);
      digits[n++] = digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + (digit - 10));
      value /= static_cast<unsigned long long>(base);
    }
  }
  while (n > 0 && out < end) {
    *out++ = digits[--n];
  }
}
}  // namespace

// std::fprintf(stderr, ...) from a raw signal handler risks silently producing no output
// at all: it goes through libc's FILE*-level lock, which -- unlike write(2) -- is not
// guaranteed async-signal-safe, and can be held by another thread (or even this same
// thread, if the signal landed mid-fprintf-call elsewhere) at the exact moment the
// handler runs. Confirmed on-device: LOG_CRITICAL calls immediately before/after
// HandleGuestSignal in this exact call chain reliably appear in the log, but the
// fprintf-based diagnostics inside this function never did, even on paths that must
// have executed.
//
// A first fix switched to vsnprintf() into a local buffer plus a raw write(2), on the
// assumption that avoiding FILE* was enough. That still produced zero output on every
// formatted call site, even after confirming (via bare write() calls with no formatting
// at all, placed at the very top of this function and at its call site) that both entry
// into this function and write(2) itself work fine in this exact context. vsnprintf is
// not actually on POSIX's async-signal-safe list either: some libc implementations
// lazily initialize per-thread locale/conversion state on first use, which can require a
// heap allocation -- and if the interrupting SIGBUS happened to land while this same
// thread already held malloc's internal lock (entirely plausible; the guest is running
// JIT'd game code that allocates constantly), that lazy init would self-deadlock forever
// against its own thread. No crash, no output, and the rest of the process (other
// threads) keeps running -- exactly what was observed. This hand-rolled formatter avoids
// vsnprintf (and malloc) entirely; it supports only the specifiers this file's call
// sites actually use (%d, %p, %#lx, %#x, %%) plus literal text.
void SignalSafeLog(const char* fmt, ...) noexcept {
  char buf[512];
  char* out = buf;
  char* const end = buf + sizeof(buf);
  va_list args;
  va_start(args, fmt);
  for (const char* p = fmt; *p != '\0' && out < end; ++p) {
    if (*p != '%') {
      *out++ = *p;
      continue;
    }
    ++p;
    if (*p == '\0') {
      break;
    }
    bool alt = false;
    if (*p == '#') {
      alt = true;
      ++p;
    }
    bool is_long = false;
    if (*p == 'l') {
      is_long = true;
      ++p;
    }
    switch (*p) {
    case 'd': {
      const int value = va_arg(args, int);
      if (value < 0) {
        if (out < end) {
          *out++ = '-';
        }
        SignalSafeWriteUnsigned(out, end, static_cast<unsigned long long>(-static_cast<long long>(value)), 10);
      } else {
        SignalSafeWriteUnsigned(out, end, static_cast<unsigned long long>(value), 10);
      }
      break;
    }
    case 'x': {
      const unsigned long long value =
          is_long ? static_cast<unsigned long long>(va_arg(args, unsigned long))
                  : static_cast<unsigned long long>(va_arg(args, unsigned int));
      if (alt) {
        if (out < end) *out++ = '0';
        if (out < end) *out++ = 'x';
      }
      SignalSafeWriteUnsigned(out, end, value, 16);
      break;
    }
    case 'p': {
      void* value = va_arg(args, void*);
      if (out < end) *out++ = '0';
      if (out < end) *out++ = 'x';
      SignalSafeWriteUnsigned(out, end, reinterpret_cast<unsigned long long>(value), 16);
      break;
    }
    case '%':
      if (out < end) *out++ = '%';
      break;
    default:
      // Unknown specifier: not expected to happen for this logger's fixed call sites,
      // but emit it verbatim rather than silently dropping it, so a mismatch is visible.
      if (out < end) *out++ = '%';
      if (*p != '\0' && out < end) *out++ = *p;
      break;
    }
  }
  va_end(args);

  size_t remaining = static_cast<size_t>(out - buf);
  const char* wp = buf;
  // write(2) can return a short count or fail with EINTR when a signal lands mid-call --
  // this is common inside a signal handler, where we may already be re-entering after
  // another interrupt. A single unchecked write() call can silently drop the whole
  // message; retry until every byte is out or a non-EINTR error occurs.
  while (remaining > 0) {
    const ssize_t written = write(STDERR_FILENO, wp, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (written == 0) {
      break;
    }
    wp += written;
    remaining -= static_cast<size_t>(written);
  }
}

bool HandleGuestSignal(int signal, siginfo_t* info, void* rawContext) noexcept {
  // Absolute minimal, unconditional entry marker: no va_args, no vsnprintf, no
  // buffer formatting -- a single hardcoded string literal and one write(2) call,
  // placed before even the __aarch64__ guard. Every previous diagnostic in this
  // function, on every code path, has produced zero output despite: the string
  // literals being confirmed present in the shipped binary (via `strings` on the
  // actual binary under test), the symbol reference to this exact function being
  // confirmed present in signals.cpp's compiled object file (so the call site is
  // definitely compiled in, not preprocessed out), and plain fprintf(stderr, ...)
  // being confirmed to work reliably on this same thread earlier in the same run
  // (376 BACHATA_FEX_HLE_* lines). If this line also never appears, the entry into
  // this function itself is broken (bad trampoline/stack state on signal delivery)
  // or write(2) is not actually able to complete during this exact signal
  // invocation -- as opposed to any bug in what this function's body does with the
  // fault info once running.
  SetHgsCheckpoint(HgsCheckpoint::Enter);
#if defined(__aarch64__)
  if (signal != SIGBUS || info == nullptr || rawContext == nullptr ||
      ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr ||
      ActiveFexExecution.SignalDelegator == nullptr) {
    SetHgsCheckpoint(HgsCheckpoint::BailoutTaken);
    return false;
  }
  SetHgsCheckpoint(HgsCheckpoint::PastBailoutCheck);

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
#ifdef __APPLE__
  auto& ts = context->uc_mcontext->__ss;
  const auto pc = static_cast<std::uintptr_t>(arm_thread_state64_get_pc(ts));
#else
  const auto pc = static_cast<std::uintptr_t>(context->uc_mcontext.pc);
#endif
  SetHgsCheckpoint(HgsCheckpoint::HavePc);
  const bool in_code_buffer =
      ActiveFexExecution.Context->IsAddressInCodeBuffer(ActiveFexExecution.Thread, pc);
  SetHgsCheckpoint(HgsCheckpoint::CheckedCodeBuffer);
  if (!in_code_buffer) {
    SetHgsCheckpoint(HgsCheckpoint::NotInCodeBufferReturn);
    return false;
  }

  if (info->si_code != BUS_ADRALN) {
    SetHgsCheckpoint(HgsCheckpoint::NotAlignmentFaultReturn);
    return false;
  }
  SetHgsCheckpoint(HgsCheckpoint::IsAlignmentFault);
#ifdef __APPLE__
  // __x[0..28] are plain (non-PAC) uint64_t registers x0-x28; fp/lr (x29/x30) are
  // separate opaque fields on Darwin, not contiguous with __x -- stage all 31 GPRs into
  // a flat buffer for HandleUnalignedAccess (which indexes it directly by instruction
  // register-encoding field, 0-30), then write back whatever it modified.
  std::array<std::uint64_t, 31> regs;
  std::memcpy(regs.data(), ts.__x, sizeof(ts.__x));
  regs[29] = static_cast<std::uint64_t>(arm_thread_state64_get_fp(ts));
  regs[30] = static_cast<std::uint64_t>(arm_thread_state64_get_lr(ts));
  // HandleUnalignedAccess backpatches the faulting JIT'd instruction in place
  // (std::atomic_ref stores directly into PC[-1..1] of whatever address it's given -- see
  // Arm64.cpp's PC[1]/PC[0]/PC[-1] stores around line 2116). On macOS, `pc` (the live,
  // executing address) doubles as the writable address too -- MAP_JIT toggles write access
  // per-thread on that SAME address via ScopedJITWriteProtect (pthread_jit_write_protect_np),
  // which is why the guard alone used to be enough there. On iOS there is no such toggle:
  // ScopedJITWriteProtect is a no-op (see AllocatorHooks.h's iOS section), and `pc` is the
  // EXECUTABLE side of a genuinely separate dual-mapped alias -- not writable at all, ever,
  // regardless of any per-thread state. Passing `pc` itself into HandleUnalignedAccess here
  // would make its backpatch stores fault the same way this whole handler exists to avoid,
  // this time with the original signal still masked (unrecoverable). Translate to the
  // writable alias first: reads/writes through it are immediately coherent with `pc` (same
  // physical pages), and the *offset* HandleUnalignedAccess returns is address-independent,
  // so resuming is still computed from the original executable `pc` below.
  SetHgsCheckpoint(HgsCheckpoint::BeforeWriteGuard);
  FEXCore::Allocator::ScopedJITWriteProtect write_guard;
  SetHgsCheckpoint(HgsCheckpoint::AfterWriteGuard);
  const uintptr_t writable_pc =
      reinterpret_cast<uintptr_t>(FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(pc)));
  SetHgsCheckpoint(HgsCheckpoint::HaveWritablePc);
  // Reading via writable_pc, not pc: the executable alias may be execute-only (no read
  // permission) on iOS, so touching it here for a diagnostic could introduce a second,
  // unrelated fault. writable_pc is known-readable since HandleUnalignedAccess writes
  // through it below.
  const uint32_t instr_before = *reinterpret_cast<volatile uint32_t*>(writable_pc);
  const auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      ActiveFexExecution.Thread,
      FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, writable_pc, regs.data());
  SetHgsCheckpoint(HgsCheckpoint::AfterHandleUnaligned);
  if (!adjustment.has_value()) {
    SetHgsCheckpoint(HgsCheckpoint::AdjustmentFailedReturn);
    SignalSafeLog("BACHATA_UNALIGNED_FAIL: pc=%p writable_pc=%p instr_before=%#x\n",
                  reinterpret_cast<void*>(pc), reinterpret_cast<void*>(writable_pc), instr_before);
    return false;
  }
  {
    const uint32_t instr_after = *reinterpret_cast<volatile uint32_t*>(writable_pc);
    SignalSafeLog("BACHATA_UNALIGNED_FIX: pc=%p writable_pc=%p adj=%d instr_before=%#x "
                  "instr_after=%#x resume_pc=%p\n",
                  reinterpret_cast<void*>(pc), reinterpret_cast<void*>(writable_pc),
                  static_cast<int>(*adjustment), instr_before, instr_after,
                  reinterpret_cast<void*>(pc + *adjustment));
    // Separate, rate-limited checkpoint so a wall-clock rate is visible directly in the log
    // without cross-referencing timestamps from other subsystems -- Rocket League was
    // observed reaching thousands of these unaligned-fix events over 80+ seconds with no
    // SubmitGfx/draw call ever following, and it wasn't clear from the existing log alone
    // whether that time is spent making steady (if slow) forward progress or stalling
    // somewhere partway through. Both BootElapsedMs (an atomic load) and SignalSafeLog are
    // async-signal-safe; the counter itself doesn't need to be, since only this one signal
    // handler ever touches it and signals of the same type don't nest on the same thread.
    static std::atomic<uint32_t> unaligned_fix_count{0};
    const uint32_t count = unaligned_fix_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % 250 == 0) {
      SignalSafeLog("BACHATA_UNALIGNED_PROGRESS: count=%d elapsed_ms=%d\n",
                    static_cast<int>(count), static_cast<int>(Common::BootElapsedMs()));
    }
  }
  // HandleUnalignedAccess just backpatched the faulting instruction (up to one instruction
  // before/after it too, for the half-barrier case) and invalidated the icache for that --
  // but only at writable_pc, the alias it actually wrote through. Execution resumes below at
  // pc, the separate executable-side alias of the same physical page: on a VA-tagged icache,
  // invalidating one alias's line does not invalidate the other's, so without this the CPU
  // can fetch whatever was cached at pc before the backpatch -- stale or unrelated
  // instructions -- and silently run off into garbage with no crash and no further syscalls,
  // rather than the freshly-patched code. Cover PC[-1] through PC[1] (3 instructions) since
  // that's the widest range HandleUnalignedAccess's backpatch touches.
  __builtin___clear_cache(reinterpret_cast<char*>(pc - 4), reinterpret_cast<char*>(pc + 8));
  std::memcpy(ts.__x, regs.data(), sizeof(ts.__x));
  arm_thread_state64_set_fp(ts, regs[29]);
  arm_thread_state64_set_lr_fptr(ts, reinterpret_cast<void*>(regs[30]));
  arm_thread_state64_set_pc_fptr(ts, reinterpret_cast<void*>(pc + *adjustment));
#else
  auto* registers = reinterpret_cast<std::uint64_t*>(context->uc_mcontext.regs);
  const auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      ActiveFexExecution.Thread,
      FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, registers);
  if (!adjustment.has_value()) {
    return false;
  }
  context->uc_mcontext.pc = pc + *adjustment;
#endif
  SetHgsCheckpoint(HgsCheckpoint::SuccessReturnTrue);
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

bool TryRecoverJitAliasFault(int signal, siginfo_t* info, void* rawContext) noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
  // A live JIT block occasionally ends up executed from its WRITABLE alias instead of the
  // EXECUTABLE one -- confirmed on-device: a Minecraft crash's own fault PC (0x11bae20b4)
  // was exactly one region-size (16384 bytes, the standard iOS JIT allocation chunk) past
  // where the region's known-good executable-side address should have put it, i.e. it sat
  // precisely at writable_base + offset instead of executable_base + offset. Something in
  // FEXCore's JIT emission still bakes in the write-side address for that one branch target
  // instead of the exec-side one -- most of this codebase's JIT-address handling was already
  // audited and fixed for this exact class of bug (CPUBackend::IsAddressInCodeBuffer,
  // Dispatcher::InitThreadPointers's Ptrs.*, JIT.cpp's EntryPoints/ExitFunctionLink/block
  // delinkers), but this crash proves at least one more site still isn't. Finding that exact
  // emission site would need tracing VIXL's own code generation; this recovers from the
  // *symptom* instead: if the fault is a plain instruction fetch (si_addr == pc, not some
  // unrelated data access -- a real wild guest pointer must NOT be silently "recovered" from
  // by this path) and PC, read as if it were a writable JIT alias, actually translates to a
  // different address that's inside a live executable code buffer, resume there instead of
  // crashing. The self-healing "re-request execute permission and retry" approach tried
  // before this (see the removed comment this replaced) failed 100% of the time for
  // already-granted regions; this doesn't re-request anything, it just corrects which alias
  // of a region that was *already* granted this thread was about to (mis)use.
  if (signal != SIGBUS && signal != SIGSEGV) {
    return false;
  }
  if (info == nullptr || rawContext == nullptr || info->si_addr == nullptr) {
    return false;
  }
  if (ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr) {
    return false;
  }

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
  auto& ts = context->uc_mcontext->__ss;
  void* const pc = reinterpret_cast<void*>(arm_thread_state64_get_pc(ts));

  if (info->si_addr != pc) {
    return false;
  }

  void* const translated = FEXCore::Allocator::GetExecutableAddress(pc);
  if (translated == pc) {
    // Not a tracked writable-alias address at all -- nothing this path can help with. This is
    // already the real safety check: GetExecutableAddress only returns something other than its
    // input for an address actually present in the table iOSJITAlloc populates, i.e. a
    // genuinely-live dual-mapped JIT region, so no separate confirmation is needed.
    //
    // Deliberately NOT also requiring IsAddressInCodeBuffer here (tried first, on-device):
    // that function only knows about the CodeBufferManager pool used for per-block guest code
    // (CurrentCodeBuffer / SignalHandlerCodeBuffers) -- it has no idea about the dispatcher's
    // own, separately-allocated bootstrap code buffer (Dispatcher.cpp's own AllocateBuffer),
    // which is exactly where this bug's fault lands (confirmed: the crashing offset falls
    // inside the dispatcher's own Start..End range). Requiring it here silently rejected the
    // one region this recovery most needed to handle.
    //
    // This `return false` was accidentally dropped in an earlier edit that removed the
    // IsAddressInCodeBuffer check above it, leaving this whole block a no-op: on ANY fault
    // where translated genuinely equals pc (a real, unrelated crash -- not this bug's target
    // at all), execution fell through to the code below anyway, logged a bogus "recovery", and
    // called arm_thread_state64_set_pc_fptr(ts, translated) with translated == pc -- i.e. set
    // the PC right back to the exact address that had just faulted. That is an infinite loop
    // by construction: immediate re-fault at the same PC, "recovered" again the same way,
    // forever. Confirmed on-device as the actual cause of every "still looping after the fix"
    // result today (millions of recoveries, pc always equal to the logged "translated" value)
    // -- independent of, and hiding the effect of, every other genuine fix made today
    // (GenerateABICall's translation, ClearCache's delinking, the buffer-reuse locking).
    return false;
  }

  SignalSafeLog("BACHATA_JIT_ALIAS_RECOVER: pc=%p (writable alias) -> resuming at %p (executable "
                "alias)\n",
                pc, translated);
  arm_thread_state64_set_pc_fptr(ts, translated);
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

// Pragmatic, narrowly-targeted recovery for a specific, known, 100%-reproducible Rocket
// League crash: a "Game" (non-main) thread reads a null-checked-but-invalid pointer while
// walking what looks like Unreal Engine 3's UProperty-style reflection-linking chain. Three
// separate diagnostic rounds (PoolCommit/PoolDecommit history, GPU page-protect history, and
// full MapMemory coverage tracking -- all in memory.h/memory.cpp, see DumpRecentPoolOps/
// DumpRecentPageProtects/DumpRecentMapOps) conclusively ruled out every VMM-level explanation:
// the backing 1.25GB "User Malloc" region is mapped by exactly one MapMemory call, with no
// gap, and nothing ever commits/decommits or reprotects any part of it afterward. The fault
// address itself is proven valid host memory; the *pointer value* the guest loaded is simply
// wrong, meaning something upstream (almost certainly in the game's own code, or a race in
// when this reflection-linking runs relative to whatever should have populated this field
// first) wrote bad data into it before this code ever ran -- a much deeper investigation than
// there's time for right now.
//
// The crash is fully deterministic in its *shape*: every occurrence is a read of
// `[rdx+0x17c]` at one of exactly two guest addresses within the same small function
// (0x7000b9ef55 and 0x7000b9ef67, both `mov reg, qword ptr [rdx+0x17c]` in the disassembled
// x86), always immediately after `test rdx,rdx; je ...` has already confirmed rdx is
// non-null. Static analysis of the surrounding x86 (see the investigation notes) confirmed
// that simply treating both reads as if they'd loaded 0 -- matching what happens elsewhere in
// this same function when the equivalent chain pointer legitimately IS null -- lets execution
// fall through to code that reconverges safely a few instructions later, with no other
// register or memory state left inconsistent. This is not a general "make null derefs survive"
// hack: it only fires for these two exact guest addresses, decoded from the live guest RIP via
// the same accurate-reconstruction mechanism used throughout this investigation
// (BachataReconstructAccurateGuestRIP), so it cannot mask an unrelated crash elsewhere.
bool TryRecoverKnownBadPropertyLink(int signal, siginfo_t* info, void* rawContext) noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
  if (signal != SIGBUS && signal != SIGSEGV) {
    return false;
  }
  if (rawContext == nullptr) {
    return false;
  }
  if (ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr) {
    return false;
  }

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
  auto& ts = context->uc_mcontext->__ss;
  void* const pc = reinterpret_cast<void*>(arm_thread_state64_get_pc(ts));

  uint64_t guest_rip = 0;
  if (!BachataReconstructAccurateGuestRIP(pc, &guest_rip)) {
    return false;
  }
  if (guest_rip != 0x7000b9ef55ULL && guest_rip != 0x7000b9ef67ULL) {
    return false;
  }

  // Read the faulting instruction to find which host register it was about to load into --
  // genuinely reading whatever FEXCore's register allocator picked this compile, rather than
  // hardcoding a register number, so this stays correct even if a future recompile (different
  // block layout, different live ranges) picks something else. Standard AArch64 load encoding:
  // the destination register is bits[4:0] regardless of the specific load variant (ldr/ldur/
  // ldapur/etc), which is all that's needed here -- this deliberately does not attempt to
  // validate the full opcode, since the guest-RIP check above already narrows this to two
  // known, previously-confirmed-by-disassembly instructions.
  const auto* writable_pc = reinterpret_cast<const volatile uint32_t*>(
      FEXCore::Allocator::GetWritableAddress(pc));
  const uint32_t instr = *writable_pc;
  const uint32_t dest_reg = instr & 0x1F;

  uint64_t old_value = 0;
  if (dest_reg == 31) {
    // Encodes XZR/SP depending on instruction class -- neither is a real destination a load
    // would target for this pattern (SP isn't a valid load destination; XZR would make the
    // load a no-op prefetch, not what's on either known crash site). Bail rather than guess.
    return false;
  }
  if (dest_reg == 29) {
    old_value = static_cast<uint64_t>(arm_thread_state64_get_fp(ts));
    arm_thread_state64_set_fp(ts, 0);
  } else if (dest_reg == 30) {
    old_value = reinterpret_cast<uint64_t>(arm_thread_state64_get_lr(ts));
    arm_thread_state64_set_lr_fptr(ts, nullptr);
  } else {
    old_value = ts.__x[dest_reg];
    ts.__x[dest_reg] = 0;
  }

  SignalSafeLog("BACHATA_PROPERTY_LINK_RECOVER: guest_rip=%p pc=%p instr=%#x dest_reg=x%d "
                "old_value=%p -> 0, resuming at %p\n",
                reinterpret_cast<void*>(guest_rip), pc, instr, static_cast<int>(dest_reg),
                reinterpret_cast<void*>(old_value), reinterpret_cast<void*>(
                    reinterpret_cast<uintptr_t>(pc) + 4));
  arm_thread_state64_set_pc_fptr(ts, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 4));
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

bool TryRecoverCallRetStackOverflow(int signal, siginfo_t* info, void* rawContext) noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
  if (signal != SIGBUS || info == nullptr || rawContext == nullptr) {
    return false;
  }
  if (ActiveFexExecution.Thread == nullptr) {
    return false;
  }

  const auto stack_base = reinterpret_cast<uintptr_t>(ActiveFexExecution.Thread->CallRetStackBase);
  if (stack_base == 0) {
    return false;
  }
  const auto fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
  // Only the guard page immediately *below* the writable region -- the direction genuine
  // exhaustion faults from (callret_sp starts high and is decremented on every guest `call`).
  // A fault at/above stack_base + CALLRET_STACK_SIZE would be a different bug (the boundary
  // issue already fixed earlier this session, see Initialize()'s own comment) and should still
  // reach the fatal path rather than being masked here.
  if (fault_addr >= stack_base || fault_addr < stack_base - static_cast<uintptr_t>(kRequiredPageSize)) {
    return false;
  }

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
  auto& ts = context->uc_mcontext->__ss;

  // REG_CALLRET_SP is a fixed physical register, not something that varies per compile (see
  // Arm64Emitter.h) -- x25 on this build (the non-arm64ec definition; arm64ec is Windows-only
  // and never applies here, but the index is still derived from the same guard FEXCore itself
  // uses rather than just trusting a bare "25" to stay in sync with it).
#ifdef ARCHITECTURE_arm64ec
  constexpr int kCallRetSpRegisterIndex = 17;
#else
  constexpr int kCallRetSpRegisterIndex = 25;
#endif
  const auto old_callret_sp = ts.__x[kCallRetSpRegisterIndex];
  // Matches CallRetStack::Initialize()'s own formula exactly: start one prediction slot
  // (0x10, the stp/ldp width) below the true top, so even an un-decremented first read from a
  // freshly-reset thread stays inside the writable region.
  const auto fresh_top =
      stack_base + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE - 0x10;
  ts.__x[kCallRetSpRegisterIndex] = static_cast<uint64_t>(fresh_top);

  SignalSafeLog("BACHATA_CALLRET_RESET: fault_addr=%p stack_base=%p old_callret_sp=%p "
                "fresh_top=%p -- resuming same instruction\n",
                info->si_addr, reinterpret_cast<void*>(stack_base),
                reinterpret_cast<void*>(old_callret_sp), reinterpret_cast<void*>(fresh_top));
  // Deliberately no PC change: the faulting instruction (a pre-indexed store, per the
  // guard-page-only check above) didn't write back its base register before faulting -- it's
  // still exactly the guest `call` that was in flight -- so simply retrying it with a valid
  // base now succeeds normally, no different from any other cache-reset push.
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

bool TryRecoverCorruptedGuestRsp(int signal, siginfo_t* info, void* rawContext) noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
  if (signal != SIGBUS || info == nullptr || rawContext == nullptr) {
    return false;
  }

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
  auto& ts = context->uc_mcontext->__ss;
  void* const pc = reinterpret_cast<void*>(arm_thread_state64_get_pc(ts));

  uint64_t guest_rip = 0;
  if (!BachataReconstructAccurateGuestRIP(pc, &guest_rip) || guest_rip != 0x7001342320ULL) {
    return false;
  }

  auto* memory = Core::Memory::Instance();
  if (memory == nullptr) {
    return false;
  }

  // x8/x9 are RSP/RBP's fixed SRA slots on this build (see TryRecoverCallRetStackOverflow's
  // own comment on how that's derived) -- reading them live here, not from CurrentFrame->
  // State's stale checkpoint copy, for the same reason BACHATA_SRA_PROBE does.
  const auto rsp_value = static_cast<VAddr>(ts.__x[8]);
  const auto rbp_value = static_cast<VAddr>(ts.__x[9]);

  ::Libraries::Kernel::OrbisVirtualQueryInfo rsp_vma {};
  ::Libraries::Kernel::OrbisVirtualQueryInfo rbp_vma {};
  const bool rsp_is_stack = memory->VirtualQuery(rsp_value, 0, &rsp_vma) == 0 && rsp_vma.is_stack != 0;
  const bool rbp_is_stack = memory->VirtualQuery(rbp_value, 0, &rbp_vma) == 0 && rbp_vma.is_stack != 0;

  // Only ever engages when rbp is confirmed trustworthy and rsp specifically is confirmed not
  // to be -- if that clean split isn't there, this stays out of the way and falls through to
  // the fatal path rather than risk masking a genuinely different bug with an unjustified guess.
  if (!rbp_is_stack || rsp_is_stack) {
    return false;
  }

  // rbp is proven to sit inside the real guest stack; give rsp a fresh position safely below
  // it instead of trying to reconstruct the exact value that was lost -- enough headroom that
  // this function's own upcoming pushes, and anything it calls, can't run back into the
  // caller's still-live frame.
  constexpr uint64_t kSafetyMargin = 0x1000;
  const auto fresh_rsp = rbp_value - kSafetyMargin;
  ts.__x[8] = static_cast<uint64_t>(fresh_rsp);

  SignalSafeLog("BACHATA_RSP_RECOVER: guest_rip=%p corrupted_rsp=%p (vma='%s') valid_rbp=%p -> "
                "fresh_rsp=%p -- resuming same instruction\n",
                reinterpret_cast<void*>(guest_rip), reinterpret_cast<void*>(rsp_value), rsp_vma.name,
                reinterpret_cast<void*>(rbp_value), reinterpret_cast<void*>(fresh_rsp));
  // Deliberately no PC change: the faulting instruction (a pre-indexed store) didn't write
  // back its base register before faulting -- retrying it with a valid base now succeeds
  // normally, no different from any other guest `push`.
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

class GuestEngine::Thread final {
public:
  Thread(std::thread::id owner, Core::GuestExecutionRequest request)
    : Owner {owner}
    , Request {std::move(request)} {}

  std::thread::id Owner;
  Core::GuestExecutionRequest Request;
  FEXCore::Core::InternalThreadState* Native {};
  CallRetStack CallRet;
  GuestSegmentState Segments;
  BridgeSyscallHandler::InvocationState Invocation;
  uint64_t FirstRip {};
  uint64_t LastRip {};
  // Set by Run() itself via pthread_self() right as this thread starts actually executing --
  // needed so the JIT buffer-invalidation safepoint (see BeginBufferInvalidationSafepoint's own
  // comment, Context.h) can pthread_kill a specific OS thread. Zero until then; safepoint code
  // must skip any thread still at zero (hasn't started running guest code yet, so it can't be
  // mid-execution of anything that needs pausing).
  std::atomic<pthread_t> NativeHandle {};
};

class GuestEngine::Impl final {
public:
  explicit Impl(GuestBridge& bridge)
    : Bridge {bridge} {}

  [[nodiscard]] EngineResult<GuestRunResult> Run() {
    if (Context == nullptr || SignalDelegator == nullptr || Syscalls == nullptr) {
      return Failure(EngineStage::Context, EINVAL);
    }
    if (Ran) {
      return Failure(EngineStage::Execute, EALREADY);
    }

    GuestCode guest = BuildGuestCode();
    if (guest.Bytes.size() > PageSize) return Failure(EngineStage::Mapping, E2BIG);
    Mapping code {PageSize, PROT_READ | PROT_WRITE};
    if (!code.IsValid()) return Failure(EngineStage::Mapping, code.Error());
    std::memcpy(code.Get(), guest.Bytes.data(), guest.Bytes.size());
    __builtin___clear_cache(reinterpret_cast<char*>(code.Get()), reinterpret_cast<char*>(code.Get()) + PageSize);
    const auto codeProtection = code.Protect(PROT_READ);
    if (const auto* error = std::get_if<EngineFailure>(&codeProtection)) return *error;

    Mapping stackPage {PageSize, PROT_READ | PROT_WRITE};
    Mapping tlsPage {PageSize, PROT_READ | PROT_WRITE};
    if (!stackPage.IsValid()) return Failure(EngineStage::Mapping, stackPage.Error());
    if (!tlsPage.IsValid()) return Failure(EngineStage::Mapping, tlsPage.Error());
    std::memcpy(tlsPage.Get(), &kThreadSentinelA, sizeof(kThreadSentinelA));

    const uint64_t initialRip = reinterpret_cast<uint64_t>(code.Get());
    CallbackReturnScope callbackReturn {*SignalDelegator, initialRip + guest.CallbackReturnOffset};
    const uint64_t initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + PageSize - 16;
    CallRetStack callRetStack;
    if (!callRetStack.IsReserved()) return Failure(EngineStage::Mapping, callRetStack.Error());
    const auto callRetStackProtection = callRetStack.MakeWritable();
    if (const auto* error = std::get_if<EngineFailure>(&callRetStackProtection)) return *error;

    auto* thread = Context->CreateThread(initialRip, initialRsp);
    if (thread == nullptr) return Failure(EngineStage::Thread, ENOMEM);
    ThreadScope threadScope {*Context, thread};
    const std::vector<Core::GuestExecutionRange> ranges {
        {static_cast<std::uintptr_t>(initialRip), PageSize, true, false},
    };
    const auto registered = Syscalls->RegisterThread(thread, ranges);
    if (const auto* failure = std::get_if<EngineFailure>(&registered)) return *failure;
    ExecutableRangeScope rangeScope {*Syscalls, thread};
    BridgeSyscallHandler::InvocationState invocation;
    BridgeSyscallHandler::InvocationScope invocationScope {*Syscalls, invocation, thread};
    GuestSegmentState segmentState;
    segmentState.Initialize(thread->CurrentFrame->State);
    callRetStack.Initialize(thread);
    thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());

    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialXmm {};
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialYmmHigh {};
    constexpr double fpLeft = 1.5;
    constexpr double fpRight = 2.25;
    const auto fpLeftBits = DoubleBits(fpLeft);
    const auto fpRightBits = DoubleBits(fpRight);
    std::memcpy(&initialXmm[0], &fpLeftBits, sizeof(fpLeftBits));
    std::memcpy(&initialXmm[1], &fpRightBits, sizeof(fpRightBits));
    Context->SetXMMRegistersFromState(thread, initialXmm.data(), HostFeatures.SupportsAVX ? initialYmmHigh.data() : nullptr);

    std::memcpy(static_cast<uint8_t*>(stackPage.Get()) + 1, &kUnalignedSentinel,
                sizeof(kUnalignedSentinel));
    thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_R12] =
        reinterpret_cast<uint64_t>(stackPage.Get()) + 1;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    if (Syscalls->FailureResult(invocation)) return *Syscalls->FailureResult(invocation);

    auto& state = thread->CurrentFrame->State;
    GuestRunResult result;
    result.Gpr = state.gregs[FEXCore::X86State::REG_R8] == kAddLeft + kAddRight &&
                 state.gregs[FEXCore::X86State::REG_R9] == (kXorLeft ^ kXorRight) &&
                 state.gregs[FEXCore::X86State::REG_RSI] == kStackSentinel &&
                 state.gregs[FEXCore::X86State::REG_RDI] == kStackSentinel &&
                 state.gregs[FEXCore::X86State::REG_RSP] == initialRsp;
    const auto rflags = Context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
    result.Rflags = (rflags & ((1U << 0) | (1U << 1) | (1U << 6) | (1U << 11))) == (1U << 1);
    result.Bridge = Syscalls->Invoked(invocation) && state.gregs[FEXCore::X86State::REG_RAX] == Syscalls->Result(invocation);
    result.Unaligned =
        state.gregs[FEXCore::X86State::REG_R13] == kUnalignedSentinel;

    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalXmm {};
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalYmmHigh {};
    Context->ReconstructXMMRegisters(thread, finalXmm.data(), HostFeatures.SupportsAVX ? finalYmmHigh.data() : nullptr);
    uint64_t fpResultBits {};
    std::memcpy(&fpResultBits, &finalXmm[0], sizeof(fpResultBits));
    result.Xmm = fpResultBits == DoubleBits(fpLeft + fpRight);
    if (!result.Gpr || !result.Rflags || !result.Xmm || !result.Bridge) {
      return Failure(EngineStage::Execute, EPROTO);
    }

    bool firstThread = false;
    bool secondThread = false;
    std::thread firstHostThread {[&] { firstThread = ExecuteThreadTlsCheck(initialRip + guest.ThreadOffset, kThreadSentinelA); }};
    std::thread secondHostThread {[&] { secondThread = ExecuteThreadTlsCheck(initialRip + guest.ThreadOffset, kThreadSentinelB); }};
    firstHostThread.join();
    secondHostThread.join();
    result.Threads = firstThread && secondThread;
    result.Tls = result.Threads;
    if (!result.Threads) return Failure(EngineStage::Thread, EPROTO);

    state.gregs[FEXCore::X86State::REG_RDI] = kCallbackInput;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->HandleCallback(thread, initialRip + guest.CallbackOffset);
    }
    if (state.gregs[FEXCore::X86State::REG_RAX] != kCallbackInput + 5 || state.gregs[FEXCore::X86State::REG_RSP] != initialRsp) {
      return Failure(EngineStage::Execute, EPROTO);
    }

    const uint64_t invalidationRip = initialRip + guest.InvalidationOffset;
    state.rip = invalidationRip;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    if (state.gregs[FEXCore::X86State::REG_RAX] != kInvalidationInitial) return Failure(EngineStage::Invalidate, EPROTO);

    auto* invalidationImmediate = static_cast<uint8_t*>(code.Get()) + guest.InvalidationImmediateOffset;
    {
      std::scoped_lock codeInvalidationLock {Context->GetCodeInvalidationMutex()};
      const auto writable = code.Protect(PROT_READ | PROT_WRITE);
      if (const auto* error = std::get_if<EngineFailure>(&writable)) return *error;
      std::memcpy(invalidationImmediate, &kInvalidationUpdated, sizeof(kInvalidationUpdated));
      __builtin___clear_cache(reinterpret_cast<char*>(code.Get()), reinterpret_cast<char*>(code.Get()) + PageSize);
      const auto executable = code.Protect(PROT_READ);
      if (const auto* error = std::get_if<EngineFailure>(&executable)) return *error;
      Context->InvalidateCodeBuffersCodeRange(invalidationRip, 11);
      Context->InvalidateThreadCachedCodeRange(thread, invalidationRip, 11);
    }
    state.rip = invalidationRip;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    result.Invalidation = state.gregs[FEXCore::X86State::REG_RAX] == kInvalidationUpdated;
    if (!result.Invalidation) return Failure(EngineStage::Invalidate, EPROTO);

    Ran = true;
    return result;
  }

  [[nodiscard]] EngineResult<bool> Shutdown() {
    {
      std::scoped_lock lock {ThreadsMutex};
      if (!Threads.empty()) {
        return Failure(EngineStage::Teardown, EBUSY);
      }
    }
    Context.reset();
    SignalDelegator.reset();
    Syscalls.reset();
    CallbackReturn.reset();
    FunctionReturn.reset();
    if (ConfigInitialized) {
      const auto configRelease = FexConfigLease::Release();
      if (const auto* failure = std::get_if<EngineFailure>(&configRelease)) return *failure;
      ConfigInitialized = false;
    }
    return true;
  }

  [[nodiscard]] bool ExecuteThreadTlsCheck(uint64_t rip, uint64_t sentinel) {
    Mapping stackPage {PageSize, PROT_READ | PROT_WRITE};
    Mapping tlsPage {PageSize, PROT_READ | PROT_WRITE};
    if (!stackPage.IsValid() || !tlsPage.IsValid()) return false;
    std::memcpy(tlsPage.Get(), &sentinel, sizeof(sentinel));
    CallRetStack callRetStack;
    if (!callRetStack.IsReserved()) return false;
    const auto callRetStackProtection = callRetStack.MakeWritable();
    if (std::holds_alternative<EngineFailure>(callRetStackProtection)) return false;
    const auto initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + PageSize - 16;
    auto* thread = Context->CreateThread(rip, initialRsp);
    if (thread == nullptr) return false;
    ThreadScope threadScope {*Context, thread};
    const std::vector<Core::GuestExecutionRange> ranges {
        {static_cast<std::uintptr_t>(rip & ~(PageSize - 1)), PageSize, true, false},
    };
    const auto registered = Syscalls->RegisterThread(thread, ranges);
    if (std::holds_alternative<EngineFailure>(registered)) return false;
    ExecutableRangeScope rangeScope {*Syscalls, thread};
    BridgeSyscallHandler::InvocationState invocation;
    BridgeSyscallHandler::InvocationScope invocationScope {*Syscalls, invocation, thread};
    GuestSegmentState segmentState;
    segmentState.Initialize(thread->CurrentFrame->State);
    callRetStack.Initialize(thread);
    thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    return thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] == sentinel &&
           thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] == initialRsp;
  }

  GuestBridge& Bridge;
  FEXCore::HostFeatures HostFeatures {};
  fextl::unique_ptr<FEXCore::Context::Context> Context;
  std::unique_ptr<BridgeSignalDelegator> SignalDelegator;
  std::unique_ptr<BridgeSyscallHandler> Syscalls;
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // iOS: trampolines need dual-mapped (RW write addr + RX exec addr) memory because
  // iOS cannot add PROT_EXEC via mprotect on unentitled processes. StikDebug must be
  // attached (via Phase 4's JIT-enablement flow) before Create() is called.
  std::unique_ptr<DualMappedMapping> CallbackReturn;
  std::unique_ptr<DualMappedMapping> FunctionReturn;
#else
  std::unique_ptr<Mapping> CallbackReturn;
  std::unique_ptr<Mapping> FunctionReturn;
#endif
  size_t PageSize {};
  bool ConfigInitialized {};
  bool Ran {};
  std::mutex ThreadsMutex;
  std::unordered_set<Thread*> Threads;
};

GuestEngine::GuestEngine(std::unique_ptr<Impl> impl)
  : ImplState {std::move(impl)} {}

GuestEngine::~GuestEngine() {
  if (ImplState != nullptr && std::holds_alternative<EngineFailure>(Shutdown())) {
    std::abort();
  }
}

EngineResult<std::unique_ptr<GuestEngine>> GuestEngine::Create(GuestBridge& bridge) {
  const auto pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize != kRequiredPageSize) return Failure(EngineStage::Mapping, ENOTSUP);

  auto impl = std::make_unique<Impl>(bridge);
  impl->PageSize = static_cast<size_t>(pageSize);
  const auto configAcquire = FexConfigLease::Acquire();
  if (const auto* failure = std::get_if<EngineFailure>(&configAcquire)) return *failure;
  impl->ConfigInitialized = true;
  const auto fail = [&impl](EngineFailure original) -> EngineResult<std::unique_ptr<GuestEngine>> {
    const auto teardown = impl->Shutdown();
    if (const auto* error = std::get_if<EngineFailure>(&teardown)) return *error;
    return original;
  };

  impl->HostFeatures = FEX::FetchHostFeatures();
  impl->Context = FEXCore::Context::Context::CreateNewContext(impl->HostFeatures);
  if (impl->Context == nullptr) {
    return fail(Failure(EngineStage::Context, ENOMEM));
  }
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // iOS dual-mapped trampolines: write through RW addr, execute via RX addr.
  // BreakGetJITMapping (brk #0xf00d, x16=1) is called inside DualMappedMapping
  // constructor — StikDebug must be attached by the time Create() runs.
  impl->FunctionReturn = std::make_unique<DualMappedMapping>(impl->PageSize);
  if (!impl->FunctionReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->FunctionReturn->Error()));
  }
  *static_cast<uint8_t*>(impl->FunctionReturn->GetRW()) = 0xf4; // HLT trampoline
  __builtin___clear_cache(static_cast<char*>(impl->FunctionReturn->GetRW()),
                          static_cast<char*>(impl->FunctionReturn->GetRW()) + impl->PageSize);
  // No mprotect needed: RX mapping is already executable (set up by BreakGetJITMapping).

  impl->CallbackReturn = std::make_unique<DualMappedMapping>(impl->PageSize);
  if (!impl->CallbackReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->CallbackReturn->Error()));
  }
  {
    auto* callbackReturn = static_cast<uint8_t*>(impl->CallbackReturn->GetRW());
    callbackReturn[0] = 0x0f; // SYSCALL trampoline (x86 opcode)
    callbackReturn[1] = 0x3e;
  }
  __builtin___clear_cache(static_cast<char*>(impl->CallbackReturn->GetRW()),
                          static_cast<char*>(impl->CallbackReturn->GetRW()) + impl->PageSize);
  // No mprotect needed: RX mapping is already executable (set up by BreakGetJITMapping).

  // Do NOT detach the debugger here. This originally assumed all DualMappedRegion
  // allocations were front-loaded before the run loop starts (see the "call once...
  // before the emulator's main run loop starts" comment in ios_jit_allocator.h), but
  // that's false: flatten_extended_userdata_pass.cpp calls DualMappedRegion::Allocate()
  // on demand throughout actual gameplay, whenever a shader needs an SRT walker JIT'd
  // for the first time. Once detached, StikDebug stops servicing the BreakGetJITMapping
  // BRK trap those later calls send -- an unhandled BRK is a raw SIGTRAP/EXC_BREAKPOINT
  // crash, not a caught error, which is exactly what killed the app ~100s into a real
  // run (long enough to hit the first shader needing fresh JIT). Staying attached for
  // the whole session is safe (see IosJitAllocator::Detach()'s own doc comment: "If not
  // called: StikDebug stays attached; this isn't fatal but wastes resources") --
  // correctness here matters more than that minor resource cost.

  // Register the RX (exec) address with the signal delegator — this is the address
  // FEXCore will branch to as the callback-return veneer.
  impl->SignalDelegator = std::make_unique<BridgeSignalDelegator>(
      reinterpret_cast<std::uintptr_t>(impl->CallbackReturn->GetRX()));
#else
  impl->FunctionReturn =
      std::make_unique<Mapping>(impl->PageSize, PROT_READ | PROT_WRITE);
  if (!impl->FunctionReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->FunctionReturn->Error()));
  }
  *static_cast<uint8_t*>(impl->FunctionReturn->Get()) = 0xf4;
  __builtin___clear_cache(static_cast<char*>(impl->FunctionReturn->Get()),
                          static_cast<char*>(impl->FunctionReturn->Get()) + impl->PageSize);
  const auto functionProtection = impl->FunctionReturn->Protect(PROT_READ | PROT_EXEC);
  if (const auto* failure = std::get_if<EngineFailure>(&functionProtection)) {
    return fail(*failure);
  }

  impl->CallbackReturn =
      std::make_unique<Mapping>(impl->PageSize, PROT_READ | PROT_WRITE);
  if (!impl->CallbackReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->CallbackReturn->Error()));
  }
  auto* callbackReturn = static_cast<uint8_t*>(impl->CallbackReturn->Get());
  callbackReturn[0] = 0x0f;
  callbackReturn[1] = 0x3e;
  __builtin___clear_cache(static_cast<char*>(impl->CallbackReturn->Get()),
                          static_cast<char*>(impl->CallbackReturn->Get()) + impl->PageSize);
  const auto callbackProtection = impl->CallbackReturn->Protect(PROT_READ | PROT_EXEC);
  if (const auto* failure = std::get_if<EngineFailure>(&callbackProtection)) {
    return fail(*failure);
  }
  impl->SignalDelegator = std::make_unique<BridgeSignalDelegator>(
      reinterpret_cast<std::uintptr_t>(impl->CallbackReturn->Get()));
#endif
  impl->Syscalls = std::make_unique<BridgeSyscallHandler>(impl->Bridge);
  impl->Context->SetSignalDelegator(impl->SignalDelegator.get());
  impl->Context->SetSyscallHandler(impl->Syscalls.get());
  impl->Context->EnableExitOnHLT();
  if (!impl->Context->InitCore()) {
    return fail(Failure(EngineStage::Context, EIO));
  }

  // See OnBufferReusedInPlace's own comment (Context.h) for the full story: on iOS, filling
  // the JIT code cache clears and reuses the same already-granted buffer in place (rather than
  // allocating a fresh one, which fails on a session's 3rd+ such request) -- and while the
  // calling thread's own fast-path lookup cache gets cleared as part of that, sibling guest
  // threads' independent caches don't, since FEXCore's own core has no list of them to reach.
  // This is the one place that list (Threads, below) actually exists, so wire it in here.
  {
    auto* rawImpl = impl.get();
    static_cast<FEXCore::Context::ContextImpl*>(impl->Context.get())->OnBufferReusedInPlace =
        [rawImpl](FEXCore::Core::InternalThreadState* CallingThread,
                  const FEXCore::LookupCacheWriteLockToken& lk) {
          LogMan::Msg::IFmt("BACHATA_BUFFER_REUSE: callback begin, CallingThread={:#x}, waiting "
                            "for ThreadsMutex",
                            reinterpret_cast<uintptr_t>(CallingThread));
          std::scoped_lock threadsLock {rawImpl->ThreadsMutex};
          LogMan::Msg::IFmt("BACHATA_BUFFER_REUSE: got ThreadsMutex, {} threads registered",
                            rawImpl->Threads.size());
          // lk is the SAME write-lock token the caller (ClearCodeCache, Core.cpp) already holds
          // -- on iOS every guest thread's LookupCache::Shared points at the one process-wide L3
          // cache tied to the single, StikDebug-count-limited JIT buffer (see JIT.cpp's
          // ThreadState->LookupCache->Shared assignment), so there is no separate per-thread
          // lock to acquire here at all; "another thread's write lock" and "the lock we already
          // hold" are literally the same mutex. An earlier version of this callback tried to
          // (re-)acquire that same lock per other-thread, which can only ever self-conflict:
          // blocking acquisition deadlocked outright, and a subsequent try-lock fallback failed
          // 100% of the time, every thread, every call, with zero real contention involved --
          // confirmed on-device by BACHATA_BUFFER_REUSE logging showing every single thread
          // reporting "busy" with no exceptions. Just reuse lk directly.
          for (auto* t : rawImpl->Threads) {
            if (t->Native == nullptr || t->Native == CallingThread) {
              // The calling thread's own L1/L2 was already cleared as part of the same
              // ClearCodeCache call that invoked this callback.
              continue;
            }
            LogMan::Msg::IFmt("BACHATA_BUFFER_REUSE: clearing thread {:#x}'s local caches",
                              reinterpret_cast<uintptr_t>(t->Native));
            t->Native->LookupCache->ClearThreadLocalCaches(lk);
          }
          LogMan::Msg::IFmt("BACHATA_BUFFER_REUSE: callback end");
        };

    // See BeginBufferInvalidationSafepoint's own comment (Context.h) and the safepoint
    // infrastructure's own comment above (SafepointSignalHandler and friends) for the full
    // story. Registered here for the same reason OnBufferReusedInPlace is: this is the one
    // place a full list of live guest threads (and now their native pthread_t handles) exists.
    EnsureSafepointHandlerInstalled();
    static_cast<FEXCore::Context::ContextImpl*>(impl->Context.get())
        ->BeginBufferInvalidationSafepoint = [rawImpl](FEXCore::Core::InternalThreadState* CallingThread) {
      LogMan::Msg::IFmt("BACHATA_SAFEPOINT: begin, CallingThread={:#x}", reinterpret_cast<uintptr_t>(CallingThread));
      g_safepoint_resume.store(false, std::memory_order_release);
      g_safepoint_paused_count.store(0, std::memory_order_release);
      int signaled = 0;
      int skipped_no_handle = 0;
      int skipped_not_started = 0;
      {
        std::scoped_lock threadsLock {rawImpl->ThreadsMutex};
        for (auto* t : rawImpl->Threads) {
          if (t->Native == nullptr || t->Native == CallingThread) {
            continue;
          }
          const pthread_t handle = t->NativeHandle.load(std::memory_order_acquire);
          if (handle == pthread_t {}) {
            ++skipped_not_started;
            continue;
          }
          if (pthread_kill(handle, kSafepointSignal) == 0) {
            ++signaled;
          } else {
            ++skipped_no_handle;
          }
        }
      }
      LogMan::Msg::IFmt("BACHATA_SAFEPOINT: signaled={} skipped_no_handle={} skipped_not_started={} hle_syscall_count={}",
                        signaled, skipped_no_handle, skipped_not_started,
                        g_threads_in_hle_syscall.load(std::memory_order_acquire));
      // Bounded wait for paused threads (SIGINFO handler). A thread parked in a genuine
      // blocking host syscall may not run the signal handler until that syscall returns on
      // its own, which could be arbitrarily long -- but such a thread isn't executing guest
      // code during that time either, so there's nothing at risk from proceeding without it.
      // Timing out is strictly no worse than this whole mechanism not existing. 2s (up from
      // 1s): unlike the HLE-syscall wait below, this one is NOT known to be futile -- a
      // signaled thread that hasn't paused yet is (by definition) still executing guest
      // code and WILL hit the signal handler on its own; the only question is how much host
      // scheduling delay it's under, which can genuinely be worse than 1s on a loaded
      // device. Every extra ms spent here is a real chance to catch a thread that the old
      // deadline was cutting off just before it would have paused.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
      while (g_safepoint_paused_count.load(std::memory_order_acquire) < signaled &&
             std::chrono::steady_clock::now() < deadline) {
        sched_yield();
      }
      int final_paused = g_safepoint_paused_count.load(std::memory_order_acquire);
      LogMan::Msg::IFmt("BACHATA_SAFEPOINT: paused wait done, final_paused={} expected={}", final_paused, signaled);
      // Additionally give any threads currently in HLE syscalls a brief window to complete.
      // These threads are executing host code (not guest JIT) and will return to guest code
      // after the syscall, so if we reuse the JIT buffer mid-syscall they could return to stale
      // code. However, g_threads_in_hle_syscall stays incremented for the ENTIRE duration of
      // Bridge.Invoke(), including HLE calls that block indefinitely (semaphore/condvar waits,
      // thread-pool idle loops - common in Unreal Engine worker threads). Those threads are not
      // about to return to guest code at all, so waiting for the counter to hit 0 can never
      // succeed for them; on-device logs confirmed a stuck count (e.g. 12) held constant for the
      // full duration of a 30s wait, meaning that wait was pure added latency with zero effect on
      // the race it targeted (the crash still happened right after). Keep only a short, best-
      // effort window to let genuinely fast/short syscalls drain without stalling gameplay for
      // threads that were never going to finish in time anyway.
      const auto hle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
      while (g_threads_in_hle_syscall.load(std::memory_order_acquire) > 0 &&
             std::chrono::steady_clock::now() < hle_deadline) {
        sched_yield();
      }
      int final_hle = g_threads_in_hle_syscall.load(std::memory_order_acquire);
      LogMan::Msg::IFmt("BACHATA_SAFEPOINT: hle wait done, final_hle_count={}", final_hle);
      // Only final_paused < signaled is actually disqualifying here -- final_hle > 0 alone is
      // not, and used to be treated as equally unsafe by mistake. A thread that confirmed
      // paused (ran the signal handler and is now blocked there on g_safepoint_resume, per
      // EndBufferInvalidationSafepoint below) cannot touch the JIT buffer again until *after*
      // this reuse and its cache clear are done, however long g_threads_in_hle_syscall stays
      // incremented for it -- that counter tracks "hasn't returned from Bridge.Invoke() yet",
      // which for a thread parked in the signal handler mid-syscall is genuinely still true,
      // but says nothing about whether it's unsafe to reuse the buffer right now. Confirmed
      // on Rocket League's log: paused=13/13 (full success) with hle_still_active=13 still
      // logged this as unsafe on every occurrence -- exactly the "stuck count held constant"
      // case the HLE-wait's own 5ms-cap comment above already describes as measuring nothing
      // useful once every thread has independently confirmed paused. This was a false alarm on
      // every occurrence observed so far (no crash followed either time), not a fix for a
      // fault -- the actual safety-relevant wait (paused < signaled) is unchanged.
      if (final_paused < signaled) {
        // Deliberately impossible to miss or mistake for routine logging: the buffer reuse
        // this safepoint exists to guard is about to happen anyway (see this call's own
        // caller, Core.cpp's ClearCodeCache -- reclaiming the buffer isn't optional once
        // growth is exhausted, so there's no safe way to abort it from here), with one or
        // more threads NOT confirmed quiesced. If a crash follows shortly after this exact
        // line in a crash log, that is not a coincidence -- it is this race. See this
        // function's own header comment for the full history of why this can't simply be
        // waited out.
        LogMan::Msg::EFmt("BACHATA_SAFEPOINT_UNSAFE_PROCEED: reusing JIT buffer WITHOUT full "
                          "quiescence -- paused={}/{} hle_still_active={} -- a crash "
                          "shortly after this line is this exact race, not a new bug",
                          final_paused, signaled, final_hle);
      } else if (final_hle > 0) {
        LogMan::Msg::IFmt("BACHATA_SAFEPOINT: proceeding, all {} threads paused ({} still "
                          "mid-HLE-call but blocked in the safepoint handler, not guest code)",
                          signaled, final_hle);
      }
    };
    static_cast<FEXCore::Context::ContextImpl*>(impl->Context.get())
        ->EndBufferInvalidationSafepoint = [](FEXCore::Core::InternalThreadState*) {
      // See g_reuse_generation's own comment for what this closes and why it has to happen
      // here specifically: after the buffer's bytes are fully repopulated (this callback fires
      // after that memcpy -- see CompileCode, JIT.cpp), before any paused thread can resume.
      const auto new_generation = g_reuse_generation.fetch_add(1, std::memory_order_release) + 1;
      // Deliberately unconditional: this whole mechanism (JITPointers::ReuseGenerationCounterAddress
      // and its two live checks, JIT.cpp/Dispatcher.cpp) has no other visibility into whether it's
      // actually wired up and firing -- a stale/never-incrementing counter would silently disable
      // both checks with no other symptom.
      LogMan::Msg::IFmt("BACHATA_REUSE_GENERATION: now {}", new_generation);
      g_safepoint_resume.store(true, std::memory_order_release);
    };
    static_cast<FEXCore::Context::ContextImpl*>(impl->Context.get())->ReuseGenerationCounter =
      &g_reuse_generation;
  }

  return std::unique_ptr<GuestEngine> {new GuestEngine {std::move(impl)}};
}

EngineResult<GuestRunResult> GuestEngine::RunControlledHarness() {
  if (ImplState == nullptr) return Failure(EngineStage::Teardown, ESHUTDOWN);
  return ImplState->Run();
}

EngineResult<GuestEngine::Thread*> GuestEngine::CreateThread(const Core::GuestExecutionRequest& request) {
  if (ImplState == nullptr || ImplState->Context == nullptr || ImplState->Syscalls == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  const auto validation = ValidateRequest(request);
  if (const auto* failure = std::get_if<EngineFailure>(&validation)) {
    return *failure;
  }

  auto thread = std::make_unique<Thread>(std::this_thread::get_id(), request);
  if (!thread->CallRet.IsReserved()) {
    return Failure(EngineStage::Mapping, thread->CallRet.Error());
  }
  const auto callRetWritable = thread->CallRet.MakeWritable();
  if (const auto* failure = std::get_if<EngineFailure>(&callRetWritable)) {
    return *failure;
  }

  thread->Native = ImplState->Context->CreateThread(request.Rip, request.Rsp);
  if (thread->Native == nullptr) {
    return Failure(EngineStage::Thread, ENOMEM);
  }

  auto& state = thread->Native->CurrentFrame->State;
  thread->Segments.Initialize(state);
  state.fs_cached = request.FsBase;
  state.gs_cached = request.GsBase;
  thread->CallRet.Initialize(thread->Native);
  state.rip = request.Rip;
  std::copy(request.Gpr.begin(), request.Gpr.end(), std::begin(state.gregs));
  state.gregs[FEXCore::X86State::REG_RSP] = request.Rsp;
  ImplState->Context->SetFlagsFromCompactedEFLAGS(thread->Native, static_cast<uint32_t>(request.Rflags));

  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  static_assert(std::tuple_size_v<decltype(request.Xmm)> == FEXCore::Core::CPUState::NUM_XMMS);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(&xmm[index], request.Xmm[index].data(), sizeof(xmm[index]));
  }
  ImplState->Context->SetXMMRegistersFromState(thread->Native, xmm.data(),
                                               ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  thread->FirstRip = request.Rip;
  const auto registration = ImplState->Syscalls->RegisterThread(thread->Native, request.MappedRanges);
  if (const auto* failure = std::get_if<EngineFailure>(&registration)) {
    ImplState->Context->DestroyThread(thread->Native);
    thread->Native = nullptr;
    return *failure;
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    const auto [_, inserted] = ImplState->Threads.insert(thread.get());
    if (!inserted) {
      ImplState->Syscalls->UnregisterThread(thread->Native);
      ImplState->Context->DestroyThread(thread->Native);
      thread->Native = nullptr;
      return Failure(EngineStage::Thread, EEXIST);
    }
  }
  return thread.release();
}

EngineResult<Core::GuestExecutionState> GuestEngine::Run(Thread& thread) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread.Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(&thread) || thread.Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
    // See NativeHandle's own comment for why this is set here, under the same lock the
    // safepoint code below reads it under.
    thread.NativeHandle.store(pthread_self(), std::memory_order_release);
  }

  BridgeSyscallHandler::InvocationScope invocationScope {*ImplState->Syscalls, thread.Invocation,
                                                          thread.Native};
  {
    FexExecutionSignalScope signalScope {*ImplState->Context, thread.Native,
                                         ImplState->SignalDelegator.get(),
                                         ImplState->Syscalls.get()};
    ImplState->Context->ExecuteThread(thread.Native);
  }
  if (ImplState->Syscalls->FailureResult(thread.Invocation)) {
    return *ImplState->Syscalls->FailureResult(thread.Invocation);
  }

  const auto& frame = thread.Native->CurrentFrame->State;
  Core::GuestExecutionState result;
  result.FirstRip = thread.FirstRip;
  result.Rip = frame.rip;
  result.Rsp = frame.gregs[FEXCore::X86State::REG_RSP];
  std::copy(std::begin(frame.gregs), std::end(frame.gregs), result.Gpr.begin());
  result.Rflags = ImplState->Context->ReconstructCompactedEFLAGS(thread.Native, false, nullptr, 0);
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  ImplState->Context->ReconstructXMMRegisters(thread.Native, xmm.data(),
                                               ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(result.Xmm[index].data(), &xmm[index], sizeof(xmm[index]));
  }
  thread.LastRip = result.Rip;
  result.LastRip = thread.LastRip;
  // This Phase-1 context exits only because EnableExitOnHLT() is active.
  result.StopReason = Core::GuestStopReason::Halted;
  return result;
}

EngineResult<Core::GuestExecutionState> GuestEngine::CallGuest(
    std::uintptr_t rip, std::span<const std::uint64_t> arguments) {
  if (ImplState == nullptr || ImplState->Context == nullptr || ImplState->Syscalls == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  auto* thread = ImplState->Syscalls->ActiveThread();
  if (thread == nullptr) return Failure(EngineStage::Thread, ENXIO);
  const auto executable = ImplState->Syscalls->QueryGuestExecutableRange(thread, rip);
  if (executable.Size == 0) return Failure(EngineStage::Mapping, EFAULT);
  if (arguments.size() > 7) return Failure(EngineStage::Request, E2BIG);

  auto& frame = thread->CurrentFrame->State;
  constexpr std::array<size_t, 6> argumentRegisters {
      FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI, FEXCore::X86State::REG_RDX,
      FEXCore::X86State::REG_RCX, FEXCore::X86State::REG_R8, FEXCore::X86State::REG_R9,
  };
  const auto registerCount = std::min(arguments.size(), argumentRegisters.size());
  for (size_t index = 0; index < registerCount; ++index) {
    frame.gregs[argumentRegisters[index]] = arguments[index];
  }
  if (arguments.size() == 7) {
    const auto rsp = frame.gregs[FEXCore::X86State::REG_RSP];
    if (rsp < sizeof(uint64_t) ||
        !ImplState->Syscalls->IsWritableRange(thread, rsp - sizeof(uint64_t),
                                               sizeof(uint64_t))) {
      return Failure(EngineStage::Mapping, EFAULT);
    }
    std::memcpy(reinterpret_cast<void*>(rsp - sizeof(uint64_t)), &arguments[6],
                sizeof(uint64_t));
  }

  BridgeSyscallHandler::InvocationState invocation;
  BridgeSyscallHandler::InvocationScope invocationScope {*ImplState->Syscalls, invocation, thread};
  {
    FexExecutionSignalScope signalScope {*ImplState->Context, thread,
                                         ImplState->SignalDelegator.get(),
                                         ImplState->Syscalls.get()};
    ImplState->Context->HandleCallback(thread, rip);
  }
  if (ImplState->Syscalls->FailureResult(invocation)) {
    return *ImplState->Syscalls->FailureResult(invocation);
  }
  if (ImplState->Syscalls->ActiveThread() != thread) {
    return Failure(EngineStage::Thread, EFAULT);
  }

  Core::GuestExecutionState result;
  result.FirstRip = rip;
  result.Rip = frame.rip;
  result.LastRip = frame.rip;
  result.Rsp = frame.gregs[FEXCore::X86State::REG_RSP];
  std::copy(std::begin(frame.gregs), std::end(frame.gregs), result.Gpr.begin());
  result.Rflags = ImplState->Context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  ImplState->Context->ReconstructXMMRegisters(
      thread, xmm.data(), ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(result.Xmm[index].data(), &xmm[index], sizeof(xmm[index]));
  }
  result.StopReason = Core::GuestStopReason::Returned;
  return result;
}

EngineResult<bool> GuestEngine::Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread.Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(&thread) || thread.Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
  }
  if (begin == 0 || size == 0 || begin % kRequiredPageSize != 0 || size % kRequiredPageSize != 0) {
    return Failure(EngineStage::Request, EINVAL);
  }
  const auto executable = std::any_of(thread.Request.MappedRanges.begin(), thread.Request.MappedRanges.end(),
                                      [begin, size](const Core::GuestExecutionRange& range) {
                                        return range.Executable && !range.Writable && Contains(range, begin, size);
                                      });
  if (!executable) {
    return Failure(EngineStage::Invalidate, EFAULT);
  }

  std::scoped_lock lock {ImplState->Context->GetCodeInvalidationMutex()};
  ImplState->Context->InvalidateCodeBuffersCodeRange(begin, size);
  ImplState->Context->InvalidateThreadCachedCodeRange(thread.Native, begin, size);
  return true;
}

EngineResult<bool> GuestEngine::DestroyThread(Thread*& thread) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread == nullptr || thread->Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(thread) || thread->Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
  }
  ImplState->Syscalls->UnregisterThread(thread->Native);
  ImplState->Context->DestroyThread(thread->Native);
  thread->Native = nullptr;
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    ImplState->Threads.erase(thread);
  }
  delete thread;
  thread = nullptr;
  return true;
}

EngineResult<bool> GuestEngine::Shutdown() {
  if (ImplState == nullptr) return true;
  auto result = ImplState->Shutdown();
  if (std::holds_alternative<bool>(result)) {
    ImplState.reset();
  }
  return result;
}

std::uintptr_t GuestEngine::ReturnAddress() const {
  if (ImplState == nullptr || ImplState->FunctionReturn == nullptr) return 0;
  return reinterpret_cast<std::uintptr_t>(ImplState->FunctionReturn->Get());
}

Core::GuestExecutionRange GuestEngine::ReturnRange() const {
  if (ImplState == nullptr || ImplState->FunctionReturn == nullptr) return {};
  return {reinterpret_cast<std::uintptr_t>(ImplState->FunctionReturn->Get()),
          ImplState->PageSize, true, false};
}

Core::GuestExecutionRange GuestEngine::CallbackReturnRange() const {
  if (ImplState == nullptr || ImplState->CallbackReturn == nullptr) return {};
  return {reinterpret_cast<std::uintptr_t>(ImplState->CallbackReturn->Get()),
          ImplState->PageSize, true, false};
}

} // namespace AetherPS4::Fex
