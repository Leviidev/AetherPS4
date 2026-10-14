// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/EnumOperators.h>
#include <FEXCore/Utils/LogManager.h>

#ifndef _WIN32
#include <stdlib.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <sys/mman.h>
#ifdef __APPLE__
#include <pthread.h>
#include <TargetConditionals.h>
#endif
#else
#define NTDDI_VERSION 0x0A000005
#include <memoryapi.h>
#endif

#include <new>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace FEXCore::Allocator {
enum class ProtectOptions : uint32_t {
  None = 0,
  Read = (1U << 0),
  Write = (1U << 1),
  Exec = (1U << 2),
};
FEX_DEF_NUM_OPS(ProtectOptions)

enum class THPControl {
  Enable,
  Disable,
};

#ifndef _WIN32
FEX_DEFAULT_VISIBILITY void SetupHooks(size_t PageSize);
#else
using VirtualNamePtr = void (*)(const char*, const void*, size_t);
using VirtualTHPPtr = void (*)(const void*, size_t, THPControl);
struct HookPtrs {
  VirtualNamePtr VirtualName;
  VirtualTHPPtr VirtualTHPControl;
};
FEX_DEFAULT_VISIBILITY void SetupHooks(size_t PageSize, HookPtrs Ptrs);
#endif
FEX_DEFAULT_VISIBILITY void ClearHooks();

#ifdef _WIN32
inline void* VirtualAlloc(void* Base, size_t Size, bool Execute = false, bool Commit = true) {
  // Allocate top-down to avoid polluting the lower VA space, as even on 64-bit some programs (i.e. LuaJIT) require allocations below 4GB.
  DWORD Flags = (Commit ? MEM_COMMIT : 0) | MEM_RESERVE | MEM_TOP_DOWN;
#ifdef ARCHITECTURE_arm64ec
  MEM_EXTENDED_PARAMETER Parameter {};
  if (Execute) {
    Parameter.Type = MemExtendedParameterAttributeFlags;
    Parameter.ULong64 = MEM_EXTENDED_PARAMETER_EC_CODE;
  };
  return ::VirtualAlloc2(nullptr, Base, Size, Flags, Execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, Execute ? &Parameter : nullptr,
                         Execute ? 1 : 0);
#else
  return ::VirtualAlloc(Base, Size, Flags, Execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#endif
}

inline void* VirtualAlloc(size_t Size, bool Execute = false, bool Commit = true) {
  return VirtualAlloc(nullptr, Size, Execute, Commit);
}

inline void VirtualFree(void* Ptr, size_t Size) {
  ::VirtualFree(Ptr, 0, MEM_RELEASE);
}

inline void VirtualDontNeed(void* Ptr, size_t Size, bool Recommit = true) {
  // Zero the page-aligned region, preserving permissions.
  MEMORY_BASIC_INFORMATION Info;
  ::VirtualQuery(Ptr, &Info, sizeof(Info));
  ::VirtualFree(Ptr, Size, MEM_DECOMMIT);
  if (Recommit) {
    ::VirtualAlloc(Ptr, Size, MEM_COMMIT, Info.Protect);
  }
}

inline bool VirtualProtect(void* Ptr, size_t Size, ProtectOptions options) {
  DWORD prot {PAGE_NOACCESS};

  if (options == ProtectOptions::None) {
    prot = PAGE_NOACCESS;
  } else if (options == ProtectOptions::Read) {
    prot = PAGE_READONLY;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Write)) {
    prot = PAGE_READWRITE;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Exec)) {
    prot = PAGE_EXECUTE_READ;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Write | ProtectOptions::Exec)) {
    prot = PAGE_EXECUTE_READWRITE;
  } else {
    LOGMAN_MSG_A_FMT("Unknown VirtualProtect options combination");
  }

  DWORD OldProt {};
  return ::VirtualProtect(Ptr, Size, prot, &OldProt) != 0;
}

FEX_DEFAULT_VISIBILITY extern VirtualNamePtr VirtualName;
FEX_DEFAULT_VISIBILITY extern VirtualTHPPtr VirtualTHPControl;
#else
using MMAP_Hook = void* (*)(void*, size_t, int, int, int, off_t);
using MUNMAP_Hook = int (*)(void*, size_t);

FEX_DEFAULT_VISIBILITY extern MMAP_Hook mmap;
FEX_DEFAULT_VISIBILITY extern MUNMAP_Hook munmap;
FEX_DEFAULT_VISIBILITY extern void VirtualName(const char* Name, void* Ptr, size_t Size);

// All commit parameters are ignored here, they are unnecessary as Linux supports overcommit

#if defined(__APPLE__) && TARGET_OS_IPHONE
// iOS has no MAP_JIT-equivalent available to a sideloaded/free-provisioned app (no
// dynamic-codesigning entitlement). Executable allocations here instead go through Bachata's
// StikDebug BreakpointJIT protocol (see iOSJITAlloc/iOSJITFreeIfOwned below, implemented in
// Allocator.cpp).
//
// Ground truth, confirmed on-device the hard way: BreakGetJITMapping's two branches are each
// broken in a complementary way for a single-address model. Requesting a grant on an address
// this process already owns (addr != nullptr) returns success but the memory never actually
// becomes executable (confirmed via raw GDB-remote wire capture -- the call completes, yet
// every subsequent branch into that memory faults, forever). Requesting a fresh allocation
// (addr == nullptr) DOES return real executable memory, but that memory is not writable from
// this process's own store instructions (confirmed via crash log: a plain write to the
// returned address SIGSEGVs from ordinary host C++ code, nowhere near guest execution). So a
// real dual-mapped scheme is required after all: get the executable address from
// BreakGetJITMapping(nullptr, ...), then locally `mach_vm_remap` a second, writable virtual
// alias of those same physical pages (no debugger involvement needed for that half -- remapping
// within one's own task is an ordinary VM operation). This matches the pattern
// Ryujinx/AetherX's DualMappedJitAllocator uses against the same protocol. GetExecutableAddress/
// GetWritableAddress below translate between the two sides via the table iOSJITAlloc populates.
// No special mmap flag is needed, so this stays 0 rather than MAP_JIT.
constexpr int PlatformExecuteFlag = 0;

// Returns the WRITABLE address of a fresh dual-mapped JIT region (write code through this
// pointer; use GetExecutableAddress() on it to get the address to branch/call into).
void* iOSJITAlloc(size_t Size);
// Returns true (and fully handles the free, both mappings) if WriteAddr is a tracked JIT
// allocation; returns false and does nothing otherwise, leaving the caller to fall through to
// an ordinary munmap.
bool iOSJITFreeIfOwned(void* WriteAddr, size_t Size);

// Real translation on iOS (unlike the identity definitions used by every other platform below):
// looks up WriteAddr/ExecAddr in the table iOSJITAlloc populates and returns the corresponding
// address on the other side of the mapping, offset-preserving (so an address partway into a
// region, e.g. a cursor mid-buffer, translates correctly, not just exact region bases). Falls
// back to identity for anything not tracked (defensive only -- every Execute=true allocation on
// iOS goes through iOSJITAlloc, so in practice every real call here should hit).
void* GetExecutableAddress(void* WriteAddr);
void* GetWritableAddress(void* ExecAddr);

enum class iOSJITAddressKind {
  NotTracked, // Not inside any currently-live JIT allocation.
  LiveAllocation,
};
// Crash-diagnostic only -- see Allocator.cpp's definition for what each result means.
iOSJITAddressKind iOSJITDescribeAddress(void* Addr, uintptr_t* OutRegionBase, size_t* OutOffset, size_t* OutSize);

// On iOS the RW half of a dual-mapped allocation is ordinary (remapped) memory -- always
// writable, no per-thread W^X state to toggle (that's a MAP_JIT-specific concept; see the
// macOS branch below). Writes go straight through the RW address at all times, so this guard
// is a no-op here.
class ScopedJITWriteProtect final {
public:
  ScopedJITWriteProtect() = default;
  ScopedJITWriteProtect(const ScopedJITWriteProtect&) = delete;
  ScopedJITWriteProtect& operator=(const ScopedJITWriteProtect&) = delete;
};
#elif defined(__APPLE__)
// Apple Silicon rejects PROT_EXEC on a plain mmap outright (EACCES) -- confirmed empirically.
// Any executable allocation must carry MAP_JIT, and writing to it afterward requires each
// writing thread to toggle pthread_jit_write_protect_np(0) first (per-thread W^X state, not
// per-mapping) -- that part is the caller's responsibility, VirtualAlloc can't do it on their
// behalf since callers write incrementally over time (e.g. a JIT emitter), not via one call
// here.
constexpr int PlatformExecuteFlag = MAP_JIT;

// RAII guard for the per-thread MAP_JIT write-protect toggle. Confirmed empirically that
// `memory region` can report a page as "rwx" while it still SIGBUS/faults on execution --
// vmmap's permission bits reflect the mapping's ceiling, not this thread's *current* W^X
// side (write-enabled+non-executable, or execute-enabled+non-writable, never both at once
// on a MAP_JIT page). Construct this around any span of code that writes JIT'd bytes into
// an Execute=true VirtualAlloc allocation; its destructor flips back to execute-enabled
// before the writing thread can fall through to running what it just emitted. Safe across
// FEXCore's setjmp-based JIT compile-restart path (CompileCode's UncheckedLongJump) since a
// longjmp back to a setjmp checkpoint *within* the same stack frame never runs destructors
// for locals constructed before that checkpoint -- only an actual `return` (or exception)
// unwinds this guard, which is exactly the "compilation attempt is over" boundary we want.
class ScopedJITWriteProtect final {
public:
  ScopedJITWriteProtect() {
    pthread_jit_write_protect_np(0);
  }
  ScopedJITWriteProtect(const ScopedJITWriteProtect&) = delete;
  ScopedJITWriteProtect& operator=(const ScopedJITWriteProtect&) = delete;
  ~ScopedJITWriteProtect() {
    pthread_jit_write_protect_np(1);
  }
};
#else
constexpr int PlatformExecuteFlag = 0;
#endif

#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
// Identity on every platform except iOS, which has a real dual-mapped RW/RX split -- see this
// file's iOS section above, which declares real (non-identity) versions of both functions
// instead of these. Kept as a named function (rather than removing every call site) so a
// JIT-buffer address that escapes as a branch target or C function pointer -- e.g. Dispatcher's
// DispatchPtr, or a compiled block's entry point before it goes into the lookup cache -- stays
// visibly marked as such at the call site on platforms where it's currently a no-op too.
inline void* GetExecutableAddress(void* WriteAddr) {
  return WriteAddr;
}

// See GetExecutableAddress.
inline void* GetWritableAddress(void* ExecAddr) {
  return ExecAddr;
}
#endif

inline void* VirtualAlloc(size_t Size, bool Execute = false, bool Commit = true) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // A plain executable mmap is rejected outright on iOS too (EACCES), same as macOS, just
  // without a MAP_JIT to reach for -- this must never fall through to the mmap below.
  if (Execute) {
    return iOSJITAlloc(Size);
  }
#endif
  return FEXCore::Allocator::mmap(nullptr, Size, PROT_READ | PROT_WRITE | (Execute ? PROT_EXEC : 0),
                                   MAP_PRIVATE | MAP_ANONYMOUS | (Execute ? PlatformExecuteFlag : 0), -1, 0);
}

inline void* VirtualAlloc(void* Base, size_t Size, bool Execute = false, bool Commit = true) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (Execute) {
    // None of this build's Execute=true callers pass a real fixed Base (verified: all either
    // omit it or pass nullptr) -- MAP_FIXED was never requested by the mmap call below either,
    // so Base was always just a hint. Safe to ignore here the same way.
    return iOSJITAlloc(Size);
  }
#endif
  return FEXCore::Allocator::mmap(Base, Size, PROT_READ | PROT_WRITE | (Execute ? PROT_EXEC : 0),
                                   MAP_PRIVATE | MAP_ANONYMOUS | (Execute ? PlatformExecuteFlag : 0), -1, 0);
}

inline void VirtualFree(void* Ptr, size_t Size) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // iOSJITFree recognizes and fully handles (munmaps) its own RW allocations, returning false
  // for anything else so the caller falls through to the ordinary munmap below -- callers here
  // don't track which VirtualAlloc path an address originally came from.
  if (iOSJITFreeIfOwned(Ptr, Size)) {
    return;
  }
#endif
  FEXCore::Allocator::munmap(Ptr, Size);
}
inline void VirtualDontNeed(void* Ptr, size_t Size, bool Recommit = true) {
  ::madvise(reinterpret_cast<void*>(Ptr), Size, MADV_DONTNEED);
}
inline bool VirtualProtect(void* Ptr, size_t Size, ProtectOptions options) {
  int prot {PROT_NONE};
  if ((options & ProtectOptions::Read) == ProtectOptions::Read) {
    prot |= PROT_READ;
  }
  if ((options & ProtectOptions::Write) == ProtectOptions::Write) {
    prot |= PROT_WRITE;
  }
  if ((options & ProtectOptions::Exec) == ProtectOptions::Exec) {
    prot |= PROT_EXEC;
  }

  return ::mprotect(Ptr, Size, prot) == 0;
}

inline void VirtualTHPControl(const void* Ptr, size_t Size, THPControl Control) {
#ifdef __APPLE__
  // Darwin's VM system manages superpages automatically; there's no per-mapping madvise hint
  // to request/decline them the way Linux's transparent-huge-pages MADV_HUGEPAGE/NOHUGEPAGE
  // does. This is purely a performance hint either way, safe to no-op.
  (void)Ptr;
  (void)Size;
  (void)Control;
#else
  ::madvise(const_cast<void*>(Ptr), Size, Control == THPControl::Enable ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#endif
}

#endif

// Memory allocation routines to be defined externally.
// This allows to use jemalloc for emulation while using the normal allocator
// for host tools without building FEXCore twice.
void* malloc(size_t size);
void* calloc(size_t n, size_t size);
void* memalign(size_t align, size_t s);
void* valloc(size_t size);
int posix_memalign(void** r, size_t a, size_t s);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
size_t malloc_usable_size(void* ptr);
void* aligned_alloc(size_t a, size_t s);
void aligned_free(void* ptr);

FEX_DEFAULT_VISIBILITY extern void InitializeThread();

#ifndef _WIN32
void SetupAllocatorHooks(void* (*)(void* addr, size_t length, int prot, int flags, int fd, off_t offset), int (*)(void* addr, size_t length));
#endif

struct FEXAllocOperators {
  FEXAllocOperators() = default;

  void* operator new(size_t size) {
    return FEXCore::Allocator::malloc(size);
  }

  void* operator new(size_t size, std::align_val_t align) {
    return FEXCore::Allocator::aligned_alloc(static_cast<size_t>(align), size);
  }

  void operator delete(void* ptr) {
    return FEXCore::Allocator::free(ptr);
  }

  void operator delete(void* ptr, std::align_val_t align) {
    return FEXCore::Allocator::aligned_free(ptr);
  }
};
} // namespace FEXCore::Allocator
