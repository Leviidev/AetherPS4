// SPDX-License-Identifier: MIT
#include "Utils/Allocator/HostAllocator.h"
#include "Utils/Allocator.h"
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/PrctlUtils.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/memory_resource.h>
#include <FEXHeaderUtils/Syscalls.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/mman.h>
#ifndef __APPLE__
#include <sys/user.h>
#endif
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <atomic>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <map>
#include <mutex>
#include <unistd.h>
#include <utility>
#endif
#endif

#ifndef MADV_DONTDUMP
// No Darwin equivalent (core-dump exclusion works differently there, via a separate
// mechanism, not a per-mapping madvise hint). The caller already treats an unsupported advice
// value as a soft failure (madvise() returns -1, feature gets disabled), so any placeholder
// value here is safe -- it never needs to actually do anything on this platform.
#define MADV_DONTDUMP 16
#endif

namespace fextl::pmr {
static fextl::pmr::default_resource FEXDefaultResource;
std::pmr::memory_resource* get_default_resource() {
  return &FEXDefaultResource;
}
} // namespace fextl::pmr

namespace FEXCore::Allocator {
#ifndef _WIN32
MMAP_Hook mmap {::mmap};
MUNMAP_Hook munmap {::munmap};

using GLIBC_MALLOC_Hook = void* (*)(size_t, const void* caller);
using GLIBC_REALLOC_Hook = void* (*)(void*, size_t, const void* caller);
using GLIBC_FREE_Hook = void (*)(void*, const void* caller);

fextl::unique_ptr<Alloc::HostAllocator> Alloc64 {};

void* FEX_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  void* Result = Alloc64->Mmap(addr, length, prot, flags, fd, offset);
  if (Result >= (void*)-4096) {
    errno = -(uint64_t)Result;
    return (void*)-1;
  }

  if (flags & MAP_ANONYMOUS) {
    VirtualName("FEXMem", Result, length);
  }
  return Result;
}

void VirtualName(const char* Name, void* Ptr, size_t Size) {
  static bool Supports {true};
  if (Supports) {
    auto Result = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, Ptr, Size, Name);
    if (Result == -1) {
      // Disable any additional attempts.
      Supports = false;
    }
  }
}

int FEX_munmap(void* addr, size_t length) {
  int Result = Alloc64->Munmap(addr, length);

  if (Result != 0) {
    errno = -Result;
    return -1;
  }
  return Result;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void AssignHookOverrides(size_t PageSize) {
  SetupAllocatorHooks(FEX_mmap, FEX_munmap);
  FEXCore::Allocator::mmap = FEX_mmap;
  FEXCore::Allocator::munmap = FEX_munmap;
  InitializeAllocator(PageSize);
}

void SetupHooks(size_t PageSize) {
  Alloc64 = Alloc::OSAllocator::Create64BitAllocator();
  AssignHookOverrides(PageSize);
}

void ClearHooks() {
  SetupAllocatorHooks(::mmap, ::munmap);
  FEXCore::Allocator::mmap = ::mmap;
  FEXCore::Allocator::munmap = ::munmap;

  Alloc::OSAllocator::ReleaseAllocatorWorkaround(std::move(Alloc64));
}
#pragma GCC diagnostic pop

FEX_DEFAULT_VISIBILITY size_t GetHostVABits() {
  static uint64_t HostVABits = 0;

  if (HostVABits) {
    return HostVABits;
  }

  static constexpr std::array<uintptr_t, 7> TLBSizes = {
    57, 52, 48, 47, 42, 39, 36,
  };

  for (auto Bits : TLBSizes) {

    // We can't actually determine VA size on ARM safely.
    // Instead, try allocating the page at the top of the range.
    // If this succeeds OR the page is reported as already existing,
    // we know we're in valid VA space. Otherwise, we must go lower.
    void* Addr = reinterpret_cast<void*>((1ULL << Bits) - FEXCore::Utils::FEX_PAGE_SIZE);
    void* Ptr = ::mmap(Addr, FEXCore::Utils::FEX_PAGE_SIZE, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Ptr != (void*)~0ULL) {
      ::munmap(Ptr, FEXCore::Utils::FEX_PAGE_SIZE);
    }
    if (Ptr != (void*)~0ULL || errno == EEXIST) {
      HostVABits = Bits;
      return Bits;
    }
  }

  LOGMAN_MSG_A_FMT("Couldn't determine host VA size");
  FEX_UNREACHABLE;
}

#define STEAL_LOG(...) // fprintf(stderr, __VA_ARGS__)

fextl::vector<MemoryRegion> CollectMemoryGaps(uintptr_t Begin, uintptr_t End, int MapsFD) {
  fextl::vector<MemoryRegion> Regions;

  uintptr_t RegionEnd = 0;

  char Buffer[2048];
  const char* Cursor = Buffer;
  ssize_t Remaining = 0;

  bool EndOfFileReached = false;

  while (true) {
    const auto line_begin = Cursor;
    auto line_end = std::find(line_begin, Cursor + Remaining, '\n');

    // Check if the buffered data covers the entire line.
    // If not, try buffering more data.
    if (line_end == Cursor + Remaining) {
      if (EndOfFileReached) {
        // No more data to buffer. Add remaining memory and return.
        const auto MapBegin = std::max(RegionEnd, Begin);
        STEAL_LOG("[%d] EndOfFile; MapBegin: %016lX MapEnd: %016lX\n", __LINE__, MapBegin, End);
        if (End > MapBegin) {
          Regions.push_back({(void*)MapBegin, End - MapBegin});
        }

        return Regions;
      }

      // Move pending content back to the beginning, then buffer more data.
      std::copy(Cursor, Cursor + Remaining, std::begin(Buffer));
      auto PendingBytes = Remaining;
      do {
        Remaining = read(MapsFD, Buffer + PendingBytes, sizeof(Buffer) - PendingBytes);
      } while (Remaining == -1 && errno == EAGAIN);

      if (Remaining < sizeof(Buffer) - PendingBytes) {
        EndOfFileReached = true;
      }

      Remaining += PendingBytes;

      Cursor = Buffer;
      continue;
    }

    // Parse mapped region in the format "fffff7cc3000-fffff7cc4000 r--p ..."
    {
      uintptr_t RegionBegin {};
      auto result = std::from_chars(Cursor, line_end, RegionBegin, 16);
      LogMan::Throw::AFmt(result.ec == std::errc {} && *result.ptr == '-', "Unexpected line format");
      Cursor = result.ptr + 1;

      // Add gap between the previous region and the current one
      const auto MapBegin = std::max(RegionEnd, Begin);
      const auto MapEnd = std::min(RegionBegin, End);
      if (MapEnd > MapBegin) {
        Regions.push_back({(void*)MapBegin, MapEnd - MapBegin});
      }

      result = std::from_chars(Cursor, line_end, RegionEnd, 16);
      LogMan::Throw::AFmt(result.ec == std::errc {} && *result.ptr == ' ', "Unexpected line format");
      Cursor = result.ptr + 1;

      STEAL_LOG("[%d] parsed line: RegionBegin=%016lX RegionEnd=%016lX\n", __LINE__, RegionBegin, RegionEnd);

      if (RegionEnd >= End) {
        // Early return if we are completely beyond the allocation space.
        return Regions;
      }
    }

    Remaining -= line_end + 1 - line_begin;
    Cursor = line_end + 1;
  }
  FEX_UNREACHABLE;
}

fextl::vector<MemoryRegion> StealMemoryRegion(uintptr_t Begin, uintptr_t End) {
  const uintptr_t StackLocation_u64 = reinterpret_cast<uintptr_t>(alloca(0));

  const int MapsFD = open("/proc/self/maps", O_RDONLY);
  LogMan::Throw::AFmt(MapsFD != -1, "Failed to open /proc/self/maps");

  auto Regions = CollectMemoryGaps(Begin, End, MapsFD);
  close(MapsFD);

  // If the memory bounds include the stack, blocking all memory regions will
  // limit the stack size to the current value. To allow some stack growth,
  // we don't block the memory gap directly below the stack memory but
  // instead map it as readable+writable.
  {
    auto StackRegionIt = std::find_if(Regions.begin(), Regions.end(), [StackLocation_u64](auto& Region) {
      return reinterpret_cast<uintptr_t>(Region.Ptr) + Region.Size > StackLocation_u64;
    });

    // If no gap crossing the stack pointer was found but the SP is within
    // the given bounds, the stack mapping is right after the last gap.
    bool IsStackMapping = StackRegionIt != Regions.end() || StackLocation_u64 <= End;

    if (IsStackMapping && StackRegionIt != Regions.begin() &&
        reinterpret_cast<uintptr_t>(std::prev(StackRegionIt)->Ptr) + std::prev(StackRegionIt)->Size <= End) {
      // Allocate the region under the stack as READ | WRITE so the stack can still grow
      --StackRegionIt;

      auto Alloc =
        ::mmap(StackRegionIt->Ptr, StackRegionIt->Size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED, -1, 0);

      LogMan::Throw::AFmt(Alloc != MAP_FAILED, "StealMemoryRegion:Stack: mmap({}, {:x}) failed: {}", fmt::ptr(StackRegionIt->Ptr),
                          StackRegionIt->Size, errno);
      LogMan::Throw::AFmt(Alloc == StackRegionIt->Ptr, "mmap returned {} instead of {}", Alloc, fmt::ptr(StackRegionIt->Ptr));

      Regions.erase(StackRegionIt);
    }
  }

  // Block remaining memory gaps
  bool SupportsDontDump = true;
  for (auto RegionIt = Regions.begin(); RegionIt != Regions.end(); ++RegionIt) {
    auto Alloc = ::mmap(RegionIt->Ptr, RegionIt->Size, PROT_NONE, MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);

    if (SupportsDontDump) {
      // Mark these regions as don't dump so that coredump doesn't try dumping large unmapped regions.
      // Ideally coredump would be smart enough to only dump resident pages, but here we are.
      auto Result = madvise(RegionIt->Ptr, RegionIt->Size, MADV_DONTDUMP);
      if (Result == -1) {
        SupportsDontDump = false;
      }
    }

    LogMan::Throw::AFmt(Alloc != MAP_FAILED, "StealMemoryRegion: mmap({}, {:x}) failed: {}", fmt::ptr(RegionIt->Ptr), RegionIt->Size, errno);
    LogMan::Throw::AFmt(Alloc == RegionIt->Ptr, "mmap returned {} instead of {}", Alloc, fmt::ptr(RegionIt->Ptr));
  }

  return Regions;
}

fextl::vector<MemoryRegion> Setup48BitAllocatorIfExists(size_t PageSize) {
  size_t Bits = FEXCore::Allocator::GetHostVABits();
  if (Bits < 48) {
    return {};
  }

  uintptr_t Begin48BitVA = 0x0'8000'0000'0000ULL;
  uintptr_t End48BitVA = 0x1'0000'0000'0000ULL;
  auto Regions = StealMemoryRegion(Begin48BitVA, End48BitVA);

  Alloc64 = Alloc::OSAllocator::Create64BitAllocatorWithRegions(Regions);
  AssignHookOverrides(PageSize);

  return Regions;
}

void ReclaimMemoryRegion(const fextl::vector<MemoryRegion>& Regions) {
  for (const auto& Region : Regions) {
    ::munmap(Region.Ptr, Region.Size);
  }
}

void LockBeforeFork(FEXCore::Core::InternalThreadState* Thread) {
  if (Alloc64) {
    Alloc64->LockBeforeFork(Thread);
  }
}

void UnlockAfterFork(FEXCore::Core::InternalThreadState* Thread, bool Child) {
  if (Alloc64) {
    Alloc64->UnlockAfterFork(Thread, Child);
  }
}
#else

void VirtualNameNOP(const char*, const void*, size_t) {}
void VirtualTHPNOP(const void* Ptr, size_t Size, THPControl Control) {}

VirtualNamePtr VirtualName {VirtualNameNOP};
VirtualTHPPtr VirtualTHPControl {VirtualTHPNOP};

void SetupHooks(size_t PageSize, HookPtrs Ptrs) {
  VirtualName = Ptrs.VirtualName;
  VirtualTHPControl = Ptrs.VirtualTHPControl;
}

#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
// iOS JIT allocation: no MAP_JIT-equivalent is available to a sideloaded/free-provisioned
// app, so executable memory instead comes from an externally-attached StikDebug session
// servicing the same BRK #0xf00d protocol as src/core/ios/ios_jit_allocator.cpp in the
// embedding app (Bachata-S4). Duplicated here rather than shared, since FEXCore-Darwin builds
// as its own standalone static library with no dependency on Bachata-S4's own Core::
// namespace.
//
// Ground truth, confirmed on-device the hard way (read from StikDebug's own source,
// github.com/StikDebug/StikDebug, plus two rounds of on-device crash evidence):
// BreakGetJITMapping(addr, len) has two branches. addr != nullptr grants execute permission on
// the caller's own pre-existing address in place -- the call returns successfully, but the
// memory never actually becomes executable (confirmed via raw GDB-remote wire capture: the
// grant call completes, yet every subsequent branch into that memory faults, forever, with zero
// memory-write packets ever sent for it). addr == nullptr instead makes the debugger allocate a
// brand-new region itself (GDB-remote "_M<len>,rx") before granting it execute permission --
// that memory genuinely IS executable, but it is NOT writable from this process's own store
// instructions (confirmed via crash log: a plain write to the returned address SIGSEGVs from
// ordinary host C++ code, nowhere near guest execution -- the "rx" in "_M<len>,rx" turns out to
// be literal). So a real dual-mapped scheme is required: get the executable address from the
// addr == nullptr branch, then locally `mach_vm_remap` a second, writable virtual alias of
// those same physical pages -- remapping within one's own task needs no debugger help or
// special entitlement, only the execute side did. This matches the pattern Ryujinx/AetherX's
// DualMappedJitAllocator uses against the same protocol, and this codebase's own
// shader-recompiler JIT (src/core/ios/ios_jit_allocator.cpp) uses the identical scheme.
namespace {
using BreakGetJITMappingFn = void* (*)(void*, size_t);
using BreakJITDetachFn = void (*)();

struct BreakpointJITSymbols {
  BreakGetJITMappingFn get_jit_mapping = nullptr;
  BreakJITDetachFn jit_detach = nullptr;
};

const BreakpointJITSymbols& GetBreakpointJITSymbols() noexcept {
  static const BreakpointJITSymbols symbols = [] {
    BreakpointJITSymbols s;
    void* handle =
      dlopen("@executable_path/Frameworks/BreakpointJIT.framework/BreakpointJIT", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      return s;
    }
    s.get_jit_mapping = reinterpret_cast<BreakGetJITMappingFn>(dlsym(handle, "BreakGetJITMapping"));
    s.jit_detach = reinterpret_cast<BreakJITDetachFn>(dlsym(handle, "BreakJITDetach"));
    return s;
  }();
  return symbols;
}

// One dual-mapped allocation: WriteBase is the mach_vm_remap'd writable alias, ExecBase is the
// address BreakGetJITMapping actually granted -- two different addresses backed by the same
// physical pages. Tracked both ways (keyed by base address -> {other side's base, size}) so
// GetExecutableAddress/GetWritableAddress can translate in either direction, and so an address
// *within* a region (not just its exact base) can still be classified/translated correctly.
// std::map (not unordered_map) specifically so upper_bound gives an ordered range lookup.
struct MappedRegion {
  uintptr_t OtherBase;
  size_t Size;
};

struct JITMappingTable {
  std::mutex Mutex;
  std::map<uintptr_t, MappedRegion> WriteToExec;
  std::map<uintptr_t, MappedRegion> ExecToWrite;
};

JITMappingTable& GetJITMappingTable() {
  static JITMappingTable Table;
  return Table;
}

// Caller must hold Table.Mutex. Returns Addr translated to the other side of the mapping it
// falls within, offset-preserving, or Addr unchanged if it isn't inside any tracked region
// (defensive fallback only -- every Execute=true allocation on iOS goes through iOSJITAlloc, so
// in practice every real call here should hit).
void* TranslateAddressLocked(const std::map<uintptr_t, MappedRegion>& Table, void* Addr) {
  if (Addr == nullptr) {
    return nullptr;
  }
  const auto A = reinterpret_cast<uintptr_t>(Addr);
  auto It = Table.upper_bound(A);
  if (It == Table.begin()) {
    return Addr;
  }
  --It;
  const auto& [Base, Region] = *It;
  if (A >= Base && A < Base + Region.Size) {
    return reinterpret_cast<void*>(Region.OtherBase + (A - Base));
  }
  return Addr;
}
} // namespace

void* GetExecutableAddress(void* WriteAddr) {
  auto& Table = GetJITMappingTable();
  std::lock_guard<std::mutex> Lock(Table.Mutex);
  return TranslateAddressLocked(Table.WriteToExec, WriteAddr);
}

void* GetWritableAddress(void* ExecAddr) {
  auto& Table = GetJITMappingTable();
  std::lock_guard<std::mutex> Lock(Table.Mutex);
  return TranslateAddressLocked(Table.ExecToWrite, ExecAddr);
}

// Counts every BreakGetJITMapping call made through iOSJITAlloc, in order, so a crash log can
// be correlated against exactly how many prior requests StikDebug had already serviced --
// distinguishing "dies after N requests" from "flaky from the start" from "always the very
// first one". Deliberately logged for every call, not just failures: a silent server-side
// failure to actually grant execute permission looks identical to success from here (see this
// file's top comment), so the request/response timeline itself is the only signal available
// short of StikDebug's own logs.
std::atomic<uint64_t> AllocationCounter{0};

void* iOSJITAlloc(size_t Size) {
  const uint64_t RequestNumber = AllocationCounter.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto& Symbols = GetBreakpointJITSymbols();
  if (Symbols.get_jit_mapping == nullptr) {
    LogMan::Msg::EFmt("iOSJITAlloc #{}: BreakpointJIT symbols unavailable", RequestNumber);
    return nullptr;
  }

  LogMan::Msg::IFmt("iOSJITAlloc #{}: requesting fresh execute-capable region (size={})",
                    RequestNumber, Size);
  void* ExecAddr = Symbols.get_jit_mapping(nullptr, Size);
  if (ExecAddr == nullptr) {
    LogMan::Msg::EFmt("iOSJITAlloc #{}: BreakGetJITMapping(nullptr, {}) returned nullptr -- StikDebug "
                      "with the Universal JIT Script must be attached before this point, or its "
                      "session has stopped responding.",
                      RequestNumber, Size);
    return nullptr;
  }
  LogMan::Msg::IFmt("iOSJITAlloc #{}: BreakGetJITMapping returned {} (execute-side)", RequestNumber,
                    ExecAddr);

  // Remap a second, writable virtual alias of the SAME physical pages, purely locally -- see
  // this file's top comment for why this half doesn't need the debugger at all. mach_vm.h's
  // 64-bit-explicit API (mach_vm_remap et al.) isn't available on iOS ("mach_vm.h unsupported"
  // from the SDK itself) -- vm_map.h's classic API works the same way here since vm_address_t
  // is pointer-width on 64-bit Darwin.
  vm_address_t WriteAddr = 0;
  vm_prot_t CurProt = VM_PROT_NONE, MaxProt = VM_PROT_NONE;
  kern_return_t Kr = vm_remap(mach_task_self(), &WriteAddr, static_cast<vm_size_t>(Size), /*mask=*/0,
                               VM_FLAGS_ANYWHERE, mach_task_self(), reinterpret_cast<vm_address_t>(ExecAddr),
                               /*copy=*/FALSE, &CurProt, &MaxProt, VM_INHERIT_NONE);
  if (Kr != KERN_SUCCESS) {
    LogMan::Msg::EFmt("iOSJITAlloc #{}: vm_remap failed for execute-side {} (size={}): "
                      "kern_return_t {} ({})",
                      RequestNumber, ExecAddr, Size, static_cast<int>(Kr), mach_error_string(Kr));
    return nullptr;
  }
  Kr = vm_protect(mach_task_self(), WriteAddr, static_cast<vm_size_t>(Size), /*set_maximum=*/FALSE,
                   VM_PROT_READ | VM_PROT_WRITE);
  if (Kr != KERN_SUCCESS) {
    LogMan::Msg::EFmt("iOSJITAlloc #{}: vm_protect(RW) failed for remapped {} (size={}): "
                      "kern_return_t {} ({})",
                      RequestNumber, reinterpret_cast<void*>(WriteAddr), Size, static_cast<int>(Kr),
                      mach_error_string(Kr));
    vm_deallocate(mach_task_self(), WriteAddr, static_cast<vm_size_t>(Size));
    return nullptr;
  }
  LogMan::Msg::IFmt("iOSJITAlloc #{}: remapped writable alias {} (execute-side {})", RequestNumber,
                    reinterpret_cast<void*>(WriteAddr), ExecAddr);

  auto& Table = GetJITMappingTable();
  std::lock_guard<std::mutex> Lock(Table.Mutex);
  Table.WriteToExec[static_cast<uintptr_t>(WriteAddr)] = MappedRegion{reinterpret_cast<uintptr_t>(ExecAddr), Size};
  Table.ExecToWrite[reinterpret_cast<uintptr_t>(ExecAddr)] = MappedRegion{static_cast<uintptr_t>(WriteAddr), Size};
  return reinterpret_cast<void*>(WriteAddr);
}

bool iOSJITFreeIfOwned(void* WriteAddr, size_t Size) {
  auto& Table = GetJITMappingTable();
  std::lock_guard<std::mutex> Lock(Table.Mutex);
  const auto It = Table.WriteToExec.find(reinterpret_cast<uintptr_t>(WriteAddr));
  if (It == Table.WriteToExec.end()) {
    return false;
  }
  const auto ExecBase = It->second.OtherBase;
  Table.WriteToExec.erase(It);
  Table.ExecToWrite.erase(ExecBase);
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(WriteAddr), static_cast<vm_size_t>(Size));
  vm_deallocate(mach_task_self(), static_cast<vm_address_t>(ExecBase), static_cast<vm_size_t>(Size));
  return true;
}

// Crash-diagnostic only: classifies an arbitrary host address against the live-allocation
// tables (checking both the write-side and execute-side, since a fault address could land on
// either) so a SIGSEGV/SIGBUS handler can log what it actually was, rather than just its raw
// bytes.
iOSJITAddressKind iOSJITDescribeAddress(void* Addr, uintptr_t* OutRegionBase, size_t* OutOffset, size_t* OutSize) {
  auto& Table = GetJITMappingTable();
  std::lock_guard<std::mutex> Lock(Table.Mutex);
  const auto A = reinterpret_cast<uintptr_t>(Addr);

  for (const auto* Side : {&Table.WriteToExec, &Table.ExecToWrite}) {
    auto It = Side->upper_bound(A);
    if (It == Side->begin()) {
      continue;
    }
    --It;
    const auto& [Base, Region] = *It;
    if (A >= Base && A < Base + Region.Size) {
      if (OutRegionBase) *OutRegionBase = Base;
      if (OutOffset) *OutOffset = A - Base;
      if (OutSize) *OutSize = Region.Size;
      return iOSJITAddressKind::LiveAllocation;
    }
  }
  return iOSJITAddressKind::NotTracked;
}
#endif
} // namespace FEXCore::Allocator
