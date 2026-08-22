# Porting Bachata S4 to native ARM64 macOS

Goal: a native `arm64-apple-darwin` build of the shadPS4 core with FEX as the x86-64→ARM64
guest CPU backend. No Rosetta 2, no x86_64 binaries, no Android/JNI, no Winlator/proot/Debian
container. iOS is explicitly out of scope for this pass.

## 1. Current architecture (as it exists today, targeting Android)

Bachata S4 is a fork of shadPS4 (upstream: https://github.com/shadps4-emu/shadPS4) with one
major addition: a real, working x86-64→ARM64 guest CPU execution path using FEX, wired in as
an embeddable library rather than FEX's normal "run a whole Linux userspace" mode. This is not
a cosmetic fork -- it's a genuine architectural extension shadPS4 upstream doesn't have.

```
                     ┌─────────────────────────────────────────────┐
                     │            Android app (Kotlin/JNI)          │
                     │        android/BachataS4/{app,core,...}      │
                     └───────────────────┬───────────────────────────┘
                                          │ launches, via a managed
                                          │ Debian/glibc chroot (Winlator-derived)
                                          ▼
              ┌───────────────────────────────────────────────────────┐
              │      shadps4 binary, built for aarch64-linux-gnu       │
              │   (runtime/scripts/build-shadps4-arm64.sh)              │
              │                                                          │
              │  src/core/*           -- OS/HLE emulation, portable C++  │
              │  src/video_core/*     -- GPU emulation (Vulkan)          │
              │  src/core/guest_cpu/  -- GuestCpuBackend abstraction     │
              │        └─ FexGuestCpuBackend  (ENABLE_FEX_GUEST_CPU)     │
              │               └─ src/core/fex/fex_guest_engine.cpp      │
              │                      └─ links FEXCore (FEX, as a        │
              │                         library, NOT full FEX/Linux)    │
              └───────────────────────────────────────────────────────┘
                                          │
                                          ▼
                     Vulkan via Mesa "KosmicKrisp" driver, OR system Vulkan
                     (on Android: Turnip/Adreno; on macOS this is already
                     the SAME driver upstream shadPS4 uses -- see below)
```

Two separate x86-on-ARM technologies exist in this repo and must not be confused:

- **FEX** (`runtime/locks/components.lock.json` pins `FEX-Emu/FEX` at `f2b679f`) is used
  *inside* shadPS4 itself, as a library (`BUILD_FEXCORE_ONLY=ON`, a Bachata-authored patch --
  `runtime/patches/fex-fexcore-only.patch` -- that strips FEX down to just the FEXCore
  translation engine, none of FEX's own Linux-application/thunk/syscall-emulation layer). This
  is the piece that actually executes PS4 game x86-64 code on ARM64. **This is the component
  we need for macOS.**
- **Box64** (`ptitSeb/box64`) is unrelated to PS4 CPU execution. It's used to run
  Winlator-derived x86_64 Linux helper binaries (X11/ALSA servers, GPU driver shims) inside the
  Android sandbox. This is Android-packaging machinery, not part of the emulation core, and is
  **not needed at all** for a native macOS build.

## 2. What's already portable, unmodified

shadPS4 upstream already supports macOS natively for everything *except* CPU execution --
confirmed by inspecting `CMakeLists.txt`:

- `if(APPLE)` blocks handle OS version minimums, `MACOSX_BUNDLE_*`, Info.plist generation.
- Vulkan on macOS is via **Mesa's KosmicKrisp driver** (`externals/mesa-kosmickrisp`), built as
  `libvulkan_kosmickrisp.dylib` + an ICD JSON and bundled with the app (see the
  `CopyVulkanDriver` custom target, ~line 1341). This is a real Gallium/Mesa driver targeting
  Metal, not MoltenVK -- shadPS4 upstream made this choice already, so **no renderer research
  or rewrite is needed here**, just reuse of what's already in the fork.
- SDL3 windowing, CoreAudio (`USE_AUDIOUNIT`), OpenCV, wolfSSL, curl, etc. -- all already have
  working Apple-target CMake paths, inherited unchanged from upstream shadPS4.

None of this needs to change. The AetherCore3 (RPCS3) work this session already validated
Homebrew-based LLVM/Qt/Vulkan-headers/MoltenVK tooling on this exact machine, which mostly
carries over.

## 3. Files that need modification

### `src/core/fex/fex_guest_engine.cpp` (1590 lines) -- the FEX↔shadPS4 bridge

Audited directly. Overwhelmingly portable POSIX C++. Exactly **two** Linux-specific call sites
found:

1. `#include <sys/mman.h>` -- POSIX-standard, exists on Darwin too. Needs verification that no
   Linux-only flags are used (e.g. `MAP_FIXED_NOREPLACE`, which doesn't exist on Darwin; Darwin
   also spells `MAP_ANONYMOUS` as `MAP_ANON` in some SDK versions, though modern Darwin accepts
   both).
2. `std::ifstream maps{"/proc/self/maps"};` -- genuinely Linux-only (procfs). This reads the
   process's own memory mappings, almost certainly to seed FEX's internal memory-region
   tracking. **Darwin replacement**: `mach_vm_region`/`vm_region_64` (Mach VM APIs) to walk the
   process's own address space, or `dyld` APIs (`_dyld_image_count`/`_dyld_get_image_*`) if this
   is really only about enumerating loaded images rather than arbitrary mapped regions -- needs
   to be determined by reading how the parsed maps are used further down in the same function.

### `src/core/guest_cpu/{fex_guest_cpu,fex_hle_bridge,hle_call_adapter}.{cpp,h}`

Not yet audited line-by-line (next step). Given how clean `fex_guest_engine.cpp` is, expect a
similarly small number of Linux-specific call sites, but this needs verification, not
assumption.

### FEXCore itself (`runtime/sources/fex`, vendored via `runtime/locks/components.lock.json`)

This is the biggest unknown and the most likely source of real blockers. FEXCore is a full
JIT compiler with its own signal-handling-based fault recovery (for guest page faults/SIGSEGV
translation), its own JIT memory allocation (RWX page management), and its own threading model.
Even with `BUILD_FEXCORE_ONLY=ON` stripping the Linux-*application* layer, FEXCore's *own*
internals were written and only ever tested against Linux. Concretely expect to find:
- Linux `signal()`/`sigaction()` usage with Linux-specific `SA_*`/`ucontext_t` field layouts
  that differ from Darwin's (Darwin's `ucontext_t`/`mcontext_t` register field names and
  offsets are different from glibc's).
- Possibly `mmap` calls relying on Linux-specific behavior (e.g. `MAP_32BIT`, huge pages via
  `MAP_HUGETLB`) for JIT code/guest memory allocation.
- Possibly direct Linux syscall numbers/`syscall()` calls for low-level operations even in the
  "core-only" build.

This needs a real build attempt against the actual FEXCore source to enumerate, not guesswork.

### `runtime/scripts/build-fexcore-smoke-aarch64.sh` -- template, not something to run as-is

This script is the extraction pattern to imitate: `checkout-component.sh` pulls FEX at the
pinned revision, applies `fex-fexcore-only.patch`, configures with
`-DBUILD_FEXCORE_ONLY=ON -DFEXCORE_PROJECT_SOURCE_DIR=... -DFEXCORE_GUEST_HARNESS_SOURCES=...`.
The macOS equivalent retargets the `-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
... aarch64-linux-gnu` cross-compile triple to a native Darwin build (no cross-compile needed
at all on Apple Silicon -- just build natively) and drops everything Android/Debian-specific.

## 4. What to discard entirely for this milestone

Not needed, not touched, for a native macOS build:
- `android/` (entire directory -- Kotlin/JNI frontend)
- `runtime/sources/box64`, `runtime/patches/box64-*.patch`, `runtime/scripts/build-box64*.sh`
- `runtime/sources/winlator-app`, Vortek client/server, X11/ALSA-over-socket protocol
- `runtime/scripts/{package-runtime,stage-debian-runtime,vendor-winlator,install-debian-runtime-deps}*`
- `runtime/locks/{winlator-vendor.sha256,debian-runtime-packages.json}`

All of that exists to bolt a Linux/glibc userspace onto Android. A native macOS build doesn't
need a "managed Linux runtime" bridge at all -- it IS the native OS, the same way upstream
shadPS4 already builds natively for macOS today (for the non-CPU-execution parts).

## 5. Exact first build target

Matching the user's stated first milestone (reach game-init, not full compatibility):

1. Build FEXCore alone for native `arm64-apple-darwin`, using
   `build-fexcore-smoke-aarch64.sh` as the template, retargeted off the Linux cross-compile
   triple. Success = `fexcore-smoke` and `fexcore-guest-harness` probes (already present in
   `runtime/probes/`) link and run natively, no Rosetta.
2. Fix whatever Darwin portability gaps step 1 surfaces in FEXCore itself (expect
   signal/ucontext and possibly JIT-memory-allocation differences -- see above).
3. Fix the `/proc/self/maps` call (and anything else found on audit) in
   `fex_guest_engine.cpp`/the other guest_cpu bridge files.
4. Configure the *main* shadps4 CMake build natively for `arm64-apple-darwin` with
   `-DENABLE_FEX_GUEST_CPU=ON`, reusing shadPS4's existing `if(APPLE)` config (SDL3,
   KosmicKrisp, CoreAudio) exactly as upstream/this fork already has it for the Apple target,
   minus anything gated behind `ARCHITECTURE STREQUAL "x86_64"` (the `cpu_patches.cpp` direct-
   execution path, which must stay off).
5. Verify with `file`/`lipo`/`otool -L` that the resulting `shadps4` binary and
   `libvulkan_kosmickrisp.dylib` are `arm64` Mach-O, and that `otool -L` shows no x86_64 slices
   and no Rosetta/`libRosetta` involvement.
6. Launch it. Success bar for this pass: the emulator initializes and reaches game-loading
   without crashing -- not that a game boots or GPU output renders correctly.

Step 1 is next.

## 6. Progress log (live)

Working tree for this: `runtime/sources/fex` (FEX checked out at the pinned revision, patched
locally, NOT the same as `runtime/patches/*.patch` -- those are Bachata's own patches for the
Android/aarch64-linux-gnu build; the Darwin-specific fixes below are local, uncommitted edits
to the checkout for now, to be turned into a tracked patch set once the build is clean).

Configure succeeded natively (`arm64-apple-darwin`, no cross-compile flags, real AppleClang)
after three shallow fixes:
- FEX's `CMakeLists.txt` had a hard platform allowlist (`Linux`/`Windows` only) -- added Darwin.
- Missing Python `packaging` module for a CPU-tuning helper script.
- Forgot to replicate Bachata's own `-DTUNE_CPU=none` flag from their build script -- without
  it, CMake tries to run a `/proc/cpuinfo`-parsing tuning script that doesn't apply on Darwin
  anyway (Apple Silicon isn't in its known-CPU-ID table regardless).

Compiling FEXCore has surfaced a real, growing list of Linux-specific APIs needing Darwin
equivalents. All of the following were verified against actual Darwin SDK headers/man pages
before fixing (not guessed), and are real edits to `runtime/sources/fex/**`:

- **`<malloc.h>`** doesn't exist on Darwin (`<malloc/malloc.h>` instead). Darwin's libc also
  lacks `memalign()`/`valloc()` entirely (not just deprecated -- absent from the SDK headers) --
  reimplemented both via `posix_memalign()`. `malloc_usable_size()` doesn't exist either;
  Darwin's equivalent is `malloc_size()`.
- **`linux/limits.h`** was only used for `PATH_MAX`, which is POSIX-standard and already in
  `<limits.h>` on every platform.
- **`prctl()`** doesn't exist on Darwin at all. Both real call sites in FEXCore's own code are
  optional/cosmetic (an MDWE hardening-status query, and naming anonymous mappings for
  `/proc/self/maps`-style debug visibility) -- both already handle a `-1`/failure return
  gracefully by design, so a stub returning `ENOSYS` is correct, not a compromise.
- **`sendfile()`** exists on Darwin but can only target a *socket* (`man sendfile`: "sends a
  regular file... out a stream socket"), not a second regular file like Linux's version.
  Replaced the one call site (whole-file copy) with a plain read/write loop.
- **`MADV_HUGEPAGE`/`MADV_NOHUGEPAGE`/`MADV_DONTDUMP`** (transparent-huge-page and core-dump
  hints) have no Darwin equivalent -- Darwin's VM manages superpages automatically with no
  per-mapping opt-in, and core-dump exclusion works through a different mechanism entirely.
  Both are pure performance/debugging hints; no-op'd on Darwin (verified `MADV_DONTDUMP`'s
  placeholder value doesn't collide with any real Darwin `MADV_*` constant).
- **`FHU::Syscalls`** (`getcpu`/`gettid`/`tgkill`/`statx`/`renameat2`/`pidfd_open`) previously
  fell through to raw `syscall(SYS_x, ...)` with Linux syscall numbers, meaningless on Darwin's
  XNU kernel. Audited actual callers within the FEXCore-only build scope: only `getcpu` (a
  CPUID-emulation helper, doesn't need to be live-accurate) and `gettid` (debug JIT-symbol
  naming only) are actually called. Implemented both for real (`gettid` via Darwin's genuine
  `pthread_threadid_np`); the other four have zero callers in this build and are stubbed.
- **`ScopedSignalMasker`** (`SignalScopeGuards.h`) used a raw `syscall(SYS_rt_sigprocmask, ...)`
  operating on a 64-bit mask. Turned out to have **zero callers anywhere in the entire FEX
  tree** -- dead code, but still needed to type-check as a non-template inline header.
  Implemented properly via `pthread_sigmask()` regardless (not just stubbed), noting clearly
  that Darwin's `sigset_t` is 32-bit vs Linux's 64-bit (realtime-signal range 33-64 isn't
  representable) in case this class ever gains a real caller later.
- **`WritePriorityMutex`** (`WritePriorityMutex.h`) -- FEXCore's hot-path lock-free
  reader-writer mutex, used pervasively by the JIT dispatcher/code-cache. This one is
  genuinely performance-critical, not a shallow API swap. It uses raw Linux `SYS_futex` with
  `FUTEX_WAIT_BITSET`/`FUTEX_WAKE_BITSET` for selective wake of readers-vs-writers sharing one
  32-bit word. The class was already designed with a portable wait/wake abstraction (there's an
  existing Windows branch using `WaitOnAddress`/`WakeByAddress*`), so adding a Darwin branch
  fit the existing pattern. Darwin has a direct, *public* equivalent for exactly this purpose:
  `os_sync_wait_on_address`/`os_sync_wake_by_address_{any,all}` (Apple's own header literally
  calls this "Darwin's futex style APIs", available since macOS 14.4, well under our 15.0
  target). One real semantic gap: no bitset-selective wake on Darwin, so a wake call wakes the
  unfiltered waiter set instead of just readers or just writers -- verified this is still
  *correct* (not just "probably fine"): every wait loop in this class already re-checks the
  atomic state and explicitly tolerates spurious wakeups by design, so the only cost is
  occasional extra wakeup/recheck cycles, never a hang or lost wakeup.

**`mremap` investigation and fix (the big one)**: `CodeCache::FinalizeCodePages` prepares
finalized/relocated JIT code in a separate "Staging" buffer, then uses
`mremap(Staging, Size, Size, MREMAP_FIXED | MREMAP_MAYMOVE | MREMAP_DONTUNMAP, CodeRange.data())`
to make it visible at a **fixed**, pre-existing address atomically. Required semantics,
determined by reading the surrounding code and its own comments (not guessed):
1. `CodeRange.data()` is a specific address inside the larger, stable `CodeBuffer` allocation
   that must not move -- per `FEXCore/Core/CodeCache.h`'s documented sequencing, blocks get
   registered to the lookup cache (`EnableLoadedSection`) *before* `FinalizeCodePages` runs, so
   other threads may already be trying to reach this exact address.
2. The transition from the `PROT_NONE` placeholder to fully-populated, executable code must be
   atomic with respect to any other thread that might read/execute nearby -- the code's own
   comment is explicit about this ("Atomicity is critical").
3. Staging's own original address must remain separately, explicitly freeable afterward
   (`Allocator::VirtualFree(Staging, Size)` runs right after) -- not implicitly torn down by
   the remap itself, matching what `MREMAP_DONTUNMAP` specifically preserves.

Investigated `mach_vm_remap` as the candidate primitive, then verified everything empirically
with standalone test programs (in `/tmp/mremap_test/`, not part of this repo) before touching
FEXCore itself, per plan:
- `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE` is Apple's own documented flag combination for
  "replace an existing mapping at this exact address" (`vm_statistics.h`: `VM_FLAGS_OVERWRITE`
  = "delete any existing mappings first").
- `mach_vm_remap(..., copy=FALSE, ...)` creates a new mapping aliasing the *same* underlying
  pages as the source, without touching the source's own mapping -- proven by test: after the
  remap, unmapping Staging's original address independently left the destination's content
  fully intact. This naturally matches `MREMAP_DONTUNMAP`'s semantics (source untouched,
  explicit separate cleanup), not plain move.
- **Real, unexpected finding**: `mach_vm_remap` unconditionally rejects any source mapping that
  includes `PROT_EXEC` with `KERN_PROTECTION_FAILURE` (verified by testing 4 variants -- JIT
  source into plain dest, JIT source into JIT-flagged dest, JIT source into freshly-freed dest,
  all failed identically; plain RW source succeeded). This has nothing to do with `MAP_JIT`
  specifically -- Darwin simply won't let you remap-in already-executable memory at all,
  seemingly to force all executable-memory transitions through the sanctioned
  `MAP_JIT`+`pthread_jit_write_protect_np` path instead.
- Also discovered along the way: plain `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, ...)` without
  `MAP_JIT` fails outright with `EACCES` on Apple Silicon -- confirmed this is a hard platform
  restriction, not an entitlement nicety (works from an unsigned/unentitled dev binary once
  `MAP_JIT` is added). And `MAP_JIT` pages enforce per-thread W^X: writing to one without first
  calling `pthread_jit_write_protect_np(0)` reliably SIGBUS'd in testing.

**The working, verified sequence** (proven by actually executing a hand-written ARM64 `RET`
instruction out of the result, not just checking return codes): allocate Staging as plain
RW (no `Execute=true`, no `MAP_JIT` -- sidesteps the PROT_EXEC-remap restriction entirely),
`mach_vm_remap` it into the fixed destination (atomic content swap, proven), then a *separate*
`mprotect(dest, size, PROT_READ|PROT_EXEC)` to flip the exec bit (also proven to work, and
also atomic in its own right since mprotect is a single kernel operation), then
`sys_icache_invalidate()` for I$/D$ coherency (ARM64 doesn't keep them coherent automatically
after writing code through the data path). No `MAP_JIT`/write-protect-toggle involvement
needed anywhere in this path, since it's a page-table swap onto fresh pages, not in-place
toggling.

Implemented in `CodeCache.cpp` (Darwin branch of `FinalizeCodePages`) and
`AllocatorHooks.h`/`.cpp` (Staging's allocation changed to `Execute=false` specifically for
this call site; `VirtualAlloc`'s general `Execute=true` path now adds `MAP_JIT` on Darwin for
*other* callers).

**Known, explicitly-deferred follow-up** (not fixed today, scope was mremap + the two smaller
items below): `Dispatcher.cpp` and `CPUBackend.cpp`'s `CodeBuffer` both allocate with
`Execute=true` and then emit code directly into the result incrementally over time (not via a
remap-swap). Since `MAP_JIT` pages need `pthread_jit_write_protect_np(0)` toggled on *before*
any write and back to `(1)` before executing, and neither call site does this yet, the very
first code emission on either path will currently SIGBUS the same way an unguarded write did
during this investigation's testing. This is the known, expected first blocker for the
standalone-JIT-test milestone, not a surprise.

**Two smaller fixes, same session**:
- `SpinWaitLock.h`'s `LoadExclusive`/`WFELoadAtomic` only had `uint8_t/16/32/64_t` overloads.
  `CodeCache.cpp` instantiates the calling template with `size_t`. On Darwin's LP64 ABI,
  `uint64_t` is `unsigned long long` while `size_t`/`unsigned long` is a *distinct* type
  despite identical width (unlike glibc, where they commonly alias to the same `unsigned long`
  typedef, so this overload gap never surfaces there). Added `size_t` overloads that delegate
  to the existing `uint64_t` ones via `reinterpret_cast` -- exact and safe, since the two types
  have identical representation on any 64-bit platform.
- `CPUBackend.cpp` had its own second, direct `#include <sys/prctl.h>`, bypassing the earlier
  fix in `PrctlUtils.h`. Same `__APPLE__` guard applied.
- `F80Fallbacks.h` called libm's `sincos()`; Darwin spells it `__sincos` (same signature,
  confirmed present in `<math.h>` since OSX 10.9).

Build progress at last check: past `CodeCache.cpp`, `CPUBackend.cpp`, `SpinWaitLock.h`, into
`OpcodeDispatcher`/`Interpreter/Fallbacks`/`Arm64Emitter`/`Dispatcher.cpp` -- the actual x86-64
interpreter and JIT emission subsystems.

## Milestone reached: standalone FEXCore JIT test executing x86-64 guest code on Apple Silicon

Both `fexcore-smoke` and `fexcore-guest-harness` now build, link, and **run to full success**
natively on Apple Silicon macOS:

```
FEXCORE_SMOKE_OK revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec gpr=ok stack=ok fp=ok threads=ok tls=ok callback=ok invalidation=ok
FEXCORE_GUEST_ENGINE_OK revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec gpr=ok rflags=ok xmm=ok bridge=ok threads=ok tls=ok unaligned=ok invalidation=ok teardown=ok
FEXCORE_GUEST_CPU_OK caller_mapping=ok thread_lifetime=ok invalidation=ok thread_isolation=ok overlap_rejected=ok nested_callback=ok
HLE_VENEER_OK scalar=ok pointer=ok function_pointer=ok vector=ok stack=ok mapping=ok
```

`file`/`lipo -info`/`otool -L` confirm both binaries are genuine `Mach-O 64-bit executable
arm64` with zero x86_64 involvement anywhere in the dependency chain (`libSystem.B.dylib`,
`libxxhash.0.dylib`, `libc++.1.dylib` only). This proves real x86-64 guest arithmetic, FP,
multi-threading, TLS, host callbacks, self-modifying-code invalidation, the guest-CPU-backend
abstraction, and the HLE veneer bridge all genuinely execute through FEXCore's JIT natively on
arm64-apple-darwin -- no Rosetta 2, no x86_64 binary anywhere.

Getting from "builds" to "runs" surfaced a long chain of real, previously-invisible runtime
bugs (build success says nothing about runtime correctness on a platform this different).
Each was root-caused with the same empirical-verification discipline as the mremap work
above, in the order encountered:

1. **AppleClang regression (build-config bug, not a portability gap)**: a mid-session `rm -rf`
   + reconfigure had dropped `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`, so CMake
   silently fell back to `/usr/bin/c++` (Xcode's AppleClang), which range-v3's `meta.hpp`
   detects via `__apple_build_version__` and routes into a legacy compatibility branch that
   conflicts with this SDK's real libc++. Fixed by reconfiguring with the compiler explicitly
   pinned again; confirmed via `CMakeCache.txt`'s `CMAKE_CXX_COMPILER` entry.
2. **Three more Linux-only files with zero consumers in `BUILD_FEXCORE_ONLY` scope**, excluded
   from their CMake source lists for Apple rather than ported (same "no in-scope consumer"
   reasoning as the FEXServer/AOT files excluded earlier): `Source/Common/FEXServerClient.cpp`
   (`ppoll`/`POLLRDHUP`/`SOCK_CLOEXEC`-based async IPC client for the out-of-process FEXServer
   daemon), `Source/Common/Linux/SBRKAllocations.cpp` (`sbrk()`/`MAP_FIXED_NOREPLACE`-based
   allocation tracking), `Source/Tools/CommonTools/Linux/Utils/ELFContainer.cpp` (`<elf.h>`
   doesn't exist on Darwin at all -- genuinely absent, not just differently-named; only used
   by the full Linux-guest-process loader, never by the FEXCORE_ONLY probes).
3. **`FEX::FetchHostFeatures()` read `ID_AA64ISAR0/1/2_EL1`, `ID_AA64PFR0/1_EL1`,
   `ID_AA64MMFR0/1/2_EL1`, and `MIDR_EL1` via raw `mrs` from EL0.** Linux's kernel traps and
   emulates EL0 reads of these ID-class registers (`Documentation/arch/arm64/cpu-feature-
   registers.rst`); XNU does not, and reading them raises SIGILL (confirmed with standalone
   single-instruction repros for each). Also `CTR_EL0` -- despite its "_EL0" name implying
   architected EL0 access -- SIGILLs on this XNU kernel too (empirically confirmed), while
   `DCZID_EL0` does not (also empirically confirmed; kept as a raw `mrs` read). Fixed by adding
   a new `CPUFeaturesFromDarwinSysctl` class that queries Apple's documented
   `sysctlbyname("hw.optional.arm.FEAT_*", ...)` feature booleans instead (verified against
   this machine's actual `sysctl -a` output, not guessed from memory), setting only features
   with a real, confirmed sysctl name and leaving everything else unset/false (SVE in
   particular -- Apple Silicon implements none of it, which correctly gates off every
   ZFR0/SVE2-derived feature downstream). `CTR_EL0`'s cache-line-size role is replaced with
   `sysctlbyname("hw.cachelinesize", ...)`; `MIDR_EL1` is left `0`, which `HandleErrata()`
   already treats as "unrecognized implementer" and safely no-ops on.
4. **Darwin's MAP_JIT write-protect is a *global per-thread* toggle, not per-mapping or
   per-allocation-call** -- `pthread_jit_write_protect_np(0/1)` affects every MAP_JIT page a
   thread owns simultaneously. This surfaced as SIGBUS/SIGSEGV at three genuinely distinct
   write sites that each needed their own toggle, discovered one at a time via `lldb`
   backtraces + `memory region` (which shows a MAP_JIT page's *declared* protection, not this
   thread's *current* W^X side -- a page can show `rwx` and still fault on write):
   - `Dispatcher::Dispatcher()`'s one-time `EmitDispatcher()` bootstrap-code emission.
   - `Arm64JITCore::CompileCode()`, the per-block JIT compiler (guard placed before its
     `UncheckedLongJump::SetJump` restart checkpoint on purpose -- a `longjmp` back to a
     `setjmp` point within the same stack frame never runs destructors for locals constructed
     before that checkpoint, only a real `return`/exception does, which is exactly the
     "compilation attempt is over" boundary wanted here).
   - `Arm64JITCore::ExitFunctionLink()` and its `DirectBlockDelinker`/`IndirectBlockDelinker`
     callbacks -- runtime inline-call-site patching (self-modifying code), invoked from the
     thread that's *executing* JIT code rather than compiling it, so `CompileCode`'s guard
     doesn't cover it.
   Fixed with a shared `FEXCore::Allocator::ScopedJITWriteProtect` RAII guard (in
   `AllocatorHooks.h`) instantiated at each site. A related bug in `runtime/probes/
   fexcore-smoke.cpp`'s own `codePage` mapping fell out of the same root cause: its
   constructor toggled write-protect once and assumed it would stay off forever (reasoning:
   this page holds guest bytes the host never executes) -- true only until FEXCore's own
   internal toggling elsewhere flipped the *global* thread state back. Fixed by re-toggling at
   the point of the second, later write instead of relying on the one-time constructor state.
5. **Host-vs-guest page-size confusion, three separate call sites**, all root-caused to the
   same fact: Apple Silicon's host page size is 16KB, not the 4KB every one of these call
   sites assumed (matching x86-64's architecturally-fixed *guest* page size, which happens to
   equal Linux's usual host page size too -- the two concepts were silently conflated
   throughout, invisible until host and guest granularity actually diverged):
   - `CPUBackend.cpp`'s `CodeBuffer` and `ThreadPoolAllocator.h`'s
     `PooledAllocatorVirtualWithGuard::Alloc` both computed their trailing guard page using
     `FEXCore::Utils::FEX_PAGE_SIZE` (4096, the guest's fixed page size) instead of the real
     host `mprotect` granularity -- `mprotect()` on Darwin rejects any address/length not
     aligned to the *actual* host page size with `EINVAL`. Worse, for `PooledAllocatorVirtual-
     WithGuard` specifically, a caller-requested `Size` smaller than one host page (common for
     this smoke test's tiny JIT staging buffer) meant the "guard page" swallowed the *entire*
     allocation, leaving zero usable bytes -- confirmed via a genuine SIGBUS overflow. Fixed by
     querying `sysconf(_SC_PAGESIZE)` and, for the pooled allocator, always over-allocating to
     guarantee at least the requested `Size` usable bytes plus a full separate host-page guard
     (`Alloc`/`Free` share one `TotalAllocSize()` helper so the real mmap'd extent used to
     unmap always matches what was actually mapped).
   - `src/core/fex/fex_guest_engine.cpp` and the `fexcore-smoke.cpp`/`fexcore-guest-harness.cpp`
     probes each had their own hardcoded `kRequiredPageSize`/`pageSize != 4096` guards, causing
     immediate `ENOTSUP`/early-return failures on a 16KB-page host before ever reaching the
     JIT. Fixed the same way: derive from the real host page size (or just drop the check
     entirely where nothing downstream actually needed the literal value 4096, only internal
     self-consistency).
6. **`CodeBuffer`'s trailing guard page turned out to be fundamentally impossible on Darwin
   for a MAP_JIT allocation**, discovered through two more empirically-confirmed restrictions
   after fixing the alignment issue above: (a) `mprotect()` refuses to change protection on a
   *sub-range* of an existing MAP_JIT mapping at all (`EACCES`, independent of alignment); (b)
   `mmap()` refuses `MAP_FIXED` combined with `MAP_JIT` outright (this build's actual failure
   was `errno=1`/`EPERM`; a standalone repro of the same combination saw `EINVAL` -- exact
   errno varies, the combination is rejected either way). A "reserve full range as PROT_NONE,
   then MAP_FIXED-overwrite the code portion with MAP_JIT" trick -- otherwise the standard,
   race-free way to place a guard page precisely adjacent to a special allocation -- is
   therefore not viable here. Since the Dispatcher's own MAP_JIT buffer already runs with no
   guard page at all and works fine, the guard page is simply omitted for this allocation on
   Darwin: a diagnostic-only safety net, not something correctness actually depends on.
   (Separately, this whole investigation surfaced that `LOGMAN_THROW_A_FMT` compiles to a
   total no-op unless `ASSERTIONS_ENABLED` -- not set in this build -- so a silently-failed
   mmap here was going unnoticed until a SIGBUS several frames away during later code
   emission. Replaced with the unconditional `ERROR_AND_DIE_FMT` for this allocation's checks.)
7. **Signal-context register access**: `src/core/fex/fex_guest_engine.cpp`'s SIGBUS/orbis-signal
   handling read `ucontext_t->uc_mcontext.pc/sp/regs` directly, Linux `mcontext_t` field names
   that don't exist on Darwin's `__darwin_mcontext64` (`uc_mcontext` is itself a pointer there,
   not an embedded struct, and pc/sp/fp/lr are pointer-authentication-opaque fields requiring
   the `arm_thread_state64_get_pc`/`get_sp`/`get_fp`/`get_lr`/`set_*_fptr` accessor macros from
   `<mach/arm/thread_status.h>` rather than direct field access). On this plain (non-arm64e)
   arm64 process the thread state carries `FLAGS_NO_PTRAUTH`, so those macros degrade to plain
   value get/set with no actual signing -- safe to use unconditionally. `x0-x28` are staged
   into a flat `uint64_t[31]` array for `HandleUnalignedAccess` (which indexes by raw
   instruction register-encoding field, 0-30) since Darwin's `fp`/`lr` aren't contiguous with
   `x0-x28` in-struct the way Linux's flat `regs[31]` array is.
8. **`ValidateHostMapping()` parsed `/proc/self/maps`**, which doesn't exist on Darwin at all
   (`std::ifstream` silently fails to open it, and this went unnoticed as a *different* bug
   because the actual failure was masked by earlier crashes until now). Ported to
   `mach_vm_region()` with `VM_REGION_BASIC_INFO_64`, walking the regions covering the
   requested range the same way the Linux version walks `/proc/self/maps` lines -- when
   `mach_vm_region()` is asked about an address inside an unmapped gap, it returns the *next*
   real region instead of failing, which is exactly how the "gap" case is detected (returned
   region starts past the address asked about).

Per the explicit scope for this milestone, shadPS4 integration was **not** started in this
section -- it begins below.

## shadPS4 integration phase begins

Two housekeeping steps first, both explicitly requested before touching shadPS4 itself:

1. **FEXCore preserved as a standalone, reusable copy.** The `runtime/sources/fex` checkout
   (a real, standalone git repo pinned at upstream FEX, not a submodule of this repo) had every
   Darwin fix from the work above sitting as uncommitted working-tree changes; these were
   committed there directly (`419b10a75 "Port FEXCore to arm64-apple-darwin (Apple Silicon)"`,
   on top of the pinned upstream `f2b679f60`), then snapshotted into a new sibling project,
   `/Users/davi/Documents/Coding/AetherCore4/FEXCore-Darwin/`, as its own independent git repo
   with a README documenting provenance and every patch. This is reusable by any future
   AetherCore project without depending on Bachata-S4's own build system or lifecycle.
2. **`Core::Fex` / `Core::GuestCpu` renamed to `AetherPS4::Fex` / `AetherPS4::GuestCpu`** --
   the combined shadPS4+FEXCore CPU/core integration namespace, per explicit instruction.
   FEXCore's own identity is untouched (still plain `FEXCore`, `FEXCore::CPU`, etc.) -- only
   the two namespaces Bachata itself carved out specifically for the bridge layer moved.
   Scope was namespace-only (no directory/file renames), chosen as the lowest-risk option
   that still keeps `fexcore-smoke`/`fexcore-guest-harness` easy to re-verify at each step.

   This turned out to be a much larger rename than the two bridge directories alone: `Core::
   Fex`/`Core::GuestCpu` (and bare `Fex::`/`GuestCpu::` reached via nested-namespace lookup
   from inside `Core`) are used pervasively throughout shadPS4's *existing* library/loader/
   linker code -- `src/core/linker.{h,cpp}`, `src/core/loader/symbols_resolver.{h,cpp}`,
   `src/core/libraries/{fios2,avplayer,system,font}/*.cpp`, `src/core/libraries/kernel/
   threads/*.cpp`, `src/core/libraries/kernel/sync/semaphore.h`, `src/main.cpp`, `src/core/
   signals.cpp` -- roughly 24 files. A namespace move is atomic (can't rename it in some
   call sites and not others without breaking the ones left behind), so all of them were
   renamed together in one pass, then re-verified.

   Two classes of mechanical mistakes surfaced doing this at scale, both caught by rebuilding
   immediately rather than trusting the sed/perl passes blind:
   - **Substring collisions**: a plain `s/Core::Fex/AetherPS4::Fex/g` also matches `Core::
     FexGuestCpuBackend` (since "Fex" is a literal prefix of "FexGuestCpuBackend"), corrupting
     it to `AetherPS4::FexGuestCpuBackend` -- but `FexGuestCpuBackend` was deliberately *not*
     moved (it's the concrete FEX backend implementation of the generic, backend-agnostic
     `Core::GuestCpuBackend` interface, kept in `Core` to stay in scope). Caught by grepping
     for `AetherPS4::Fex[A-Za-z]` (i.e. anything after "Fex" that isn't `::`) post-rename and
     reverting the false positives.
   - **Broken nested-namespace lookup, in both directions**: moving `Core::Fex` out from
     under `Core` breaks any *bare* reference to a `Core`-level type used from code that used
     to live inside that nesting (e.g. `fex_guest_engine.cpp`, now in `AetherPS4::Fex`, had
     bare `GuestExecutionRange`/`GuestExecutionRequest`/`GuestExecutionState`/`GuestStopReason`
     relying on old enclosing-namespace lookup into `Core`) -- these needed explicit `Core::`
     qualification. The mirror-image mistake also happened: a blanket qualification pass over
     `fex_guest_cpu.{h,cpp}` (which never moved -- they're still natively inside plain
     `namespace Core { ... }`) incorrectly qualified *self*-references like `class
     FexGuestCpuBackend` and the out-of-line `FexGuestCpuBackend::FexGuestCpuBackend(...)`
     constructor definition with their own enclosing namespace, producing invalid syntax
     (`class Core::FexGuestCpuBackend` is not a legal class definition from inside `namespace
     Core { ... }`). Both directions were caught immediately by the compiler once rebuilt.

   `fexcore-smoke` and `fexcore-guest-harness` were rebuilt and re-verified passing (full
   `FEXCORE_SMOKE_OK`/`FEXCORE_GUEST_ENGINE_OK`/`FEXCORE_GUEST_CPU_OK`/`HLE_VENEER_OK`) after
   the rename settled. The ~15 renamed files outside that build's compile set (`linker.*`,
   `symbols_resolver.*`, the `src/core/libraries/*` call sites) can't be compile-verified
   until shadPS4's own CMake target builds them, which is the next step below.

### shadPS4's own CMake already has FEX-guest-CPU wiring

Before touching anything, the top-level `CMakeLists.txt` turned out to already contain a
complete, pre-existing `ENABLE_FEX_GUEST_CPU` option (added by Bachata for the Android/generic-
ARM64 target, not something this session needs to invent): it lists the exact static libraries
FEXCore's `BUILD_FEXCORE_ONLY` build produces, all the include paths, and the compile
definitions/link flags needed, gated on `ARCHITECTURE STREQUAL "arm64"` (platform-agnostic --
doesn't exclude `APPLE`). Also confirmed via this file: shadPS4 upstream already has real,
non-trivial Apple support (`if (APPLE)` blocks bundling `libvulkan_kosmickrisp.dylib` next to
the built app, an `.mm` window source file, `CMAKE_OSX_DEPLOYMENT_TARGET`), consistent with
the earlier finding that macOS/KosmicKrisp Vulkan needs no rewrite.

`FEXCORE_GUEST_CPU_BUILD_DIR` (default `runtime/build/fexcore-smoke-build`, a different
directory than the one used throughout this port) was pointed at this session's actual
`runtime/build/fexcore-macos-build`. Comparing its `FEXCORE_GUEST_CPU_LIBRARIES` list against
what our build produces found exactly one mismatch: `libxxhash.a` was missing, because the
Darwin configure had been resolving xxhash via `find_package(xxhash MODULE QUIET)` against
Homebrew's system copy rather than building FEX's own vendored `External/xxhash` submodule
copy the top-level build expects. Fixed by reconfiguring with
`-DCMAKE_DISABLE_FIND_PACKAGE_xxhash=ON`, forcing the vendored subdirectory build; re-verified
`fexcore-smoke`/`fexcore-guest-harness` still pass after the reconfigure.

shadPS4's dependency submodules (`externals/*`, 44 of them -- Vulkan headers, SDL3, FFmpeg,
Boost, glslang, etc.) were all uninitialized going into this and needed fetching first,
including several *nested* submodules CMake only reveals one at a time as configure reaches
each subproject (`externals/mesa-kosmickrisp/externals/mesa` -- the actual KosmicKrisp
Vulkan-on-Metal driver source, hit a transient `gitlab.freedesktop.org` 502 outage,
confirmed via direct `curl`, resolved on retry with no code changes needed; `externals/
sirit/externals/SPIRV-Headers`, `externals/discord-rpc/thirdparty/rapidjson`, `externals/
freetype/subprojects/dlg`).

One genuine missing-file gap: `src/dist/MacOSBundleInfo.plist.in` (the macOS app-bundle
Info.plist template `configure_file`'d and linker-embedded via `-sectcreate __TEXT
__info_plist`) doesn't exist in this fork at all, despite the surrounding `if (APPLE)` CMake
logic already assuming it's there. Restored verbatim from upstream shadps4-emu/shadPS4 (not
guessed -- fetched via `raw.githubusercontent.com`), unmodified: it already parameterizes via
`${APP_VERSION}`/`${APP_VERSION_NUM}`/`${CMAKE_OSX_DEPLOYMENT_TARGET}`, all of which this
fork's `CMakeLists.txt` already defines identically to upstream.

**`cmake -S . -B runtime/build/shadps4-macos-arm64` for `arm64-apple-darwin` now configures
completely clean** (`-- Configuring done` / `-- Generating done`), with `ENABLE_FEX_GUEST_CPU=ON`
pointed at this session's Darwin FEXCore build -- the full dependency graph (KosmicKrisp
Vulkan, SDL3, FFmpeg, Boost, spdlog, and everything else) resolves.

## Milestone reached: shadps4 builds, links, and runs natively on arm64-apple-darwin

```
$ file runtime/build/shadps4-macos-arm64/shadps4
runtime/build/shadps4-macos-arm64/shadps4: Mach-O 64-bit executable arm64
$ lipo -archs runtime/build/shadps4-macos-arm64/shadps4
arm64
$ runtime/build/shadps4-macos-arm64/shadps4 --help
shadPS4 Emulator CLI
...
```

A genuine, complete `shadps4` executable -- shadPS4's own full application (loader, video_core,
shader_recompiler, every `core/libraries/*` syscall implementation, the KosmicKrisp
Vulkan-on-Metal driver built from source, and the FEXCore/AetherPS4 guest-CPU integration all
linked together) -- builds, links, and runs on real Apple Silicon hardware. `otool -L` confirms
only real macOS system frameworks (CoreAudio, AudioToolbox, Metal, Cocoa, IOKit, ...) and no
x86_64 anywhere. `fexcore-smoke` and `fexcore-guest-harness` both still pass completely,
confirming the FEXCore work from earlier in this port was untouched by anything below.

Getting the ~1900-step full build (KosmicKrisp Mesa driver + every shadPS4 dependency +
shadPS4 itself) to a clean link surfaced a further round of real, previously-unreachable
issues, in the order hit:

1. **Missing host build tools for the KosmicKrisp Mesa driver's own Meson-based build**
   (wrapped by CMake's `ExternalProject_Add`, invoked automatically as part of the main
   build, not something run separately): `meson` itself, `libclc` (LLVM's OpenCL-C runtime,
   needed by `mesa_clc`), the `mako` and `pyyaml` Python modules (Meson's own dependency
   checks for Mesa's code generation, failing with an unhelpful "Python >= 3.10 not found"
   even though a valid 3.14 interpreter existed -- the real failure was two `import` checks
   inside Meson's version-probe script), and `spirv-llvm-translator` / `spirv-tools`
   (`LLVMSPIRVLib`/`SPIRV-Tools`, needed by the SPIR-V compilation pipeline). All installed
   via Homebrew/pip; none needed for FEXCore's own build, only Mesa's.
2. **Nested git submodules CMake only reveals one at a time**, as configure reaches each
   subproject: `externals/mesa-kosmickrisp/externals/mesa` (the actual KosmicKrisp driver
   source; hit a transient `gitlab.freedesktop.org` 502 outage along the way, confirmed via
   direct `curl`, resolved on retry with zero code changes), `externals/sirit/externals/
   SPIRV-Headers`, `externals/discord-rpc/thirdparty/rapidjson`, `externals/freetype/
   subprojects/dlg`.
3. **`src/dist/MacOSBundleInfo.plist.in` doesn't exist in this fork** despite the surrounding
   `if (APPLE)` CMake logic already assuming it's there (linker-embeds it via `-sectcreate
   __TEXT __info_plist`). Restored verbatim from upstream shadps4-emu/shadPS4 (fetched via
   `raw.githubusercontent.com`, not guessed) -- it was already parameterized via
   `${APP_VERSION}`/`${APP_VERSION_NUM}`/`${CMAKE_OSX_DEPLOYMENT_TARGET}`, all already defined
   identically in this fork's own `CMakeLists.txt`.
4. **A `<float.h>` include-guard collision, systemic across the whole build, not just
   FEXCore**: this SDK ships two distinct `<float.h>` files sharing one include guard (Clang's
   minimal freestanding resource-dir copy and the full platform SDK copy); whichever gets
   pulled in first via a transitive include wins the guard race, and Clang's copy lacks
   `DBL_DIG`/`FLT_MIN`/`FLT_MAX`/etc. First hit in `externals/pugixml/src/pugixml.cpp`
   (bisected the exact triggering header empirically: `<stdlib.h>` included before `<float.h>`
   pulls in the incomplete copy first) and patched locally there (also happens to fix a real
   latent upstream bug: pugixml's own `<float.h>` include is conditional on
   `!PUGIXML_NO_XPATH`, but the code using `DBL_DIG` isn't). Confirmed via a bisected
   standalone repro that this is generic, not pugixml-specific, so fixed globally via
   `-include float.h` in `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`/`CMAKE_OBJC_FLAGS`/
   `CMAKE_OBJCXX_FLAGS` rather than patching every affected file individually.
5. **Several Apple-clang-fork-specific predefined macros that vanilla Homebrew LLVM never
   defines**, each surfacing as `-Werror,-Wundef` in a different Apple system framework header
   on this bleeding-edge beta SDK, as different files reached different transitive includes:
   `__SWIFT_ATTR_SUPPORTS_SENDABLE_DECLS` (CoreFoundation), `__OBJC2__` (always `1` on 64-bit
   Apple platforms -- AudioToolbox). Defined globally alongside the `float.h` fix rather than
   fought via flag ordering (a `-Wno-error=undef` in `CMAKE_CXX_FLAGS` doesn't survive a
   *later*, target-specific `-Werror=undef` on the same command line -- last flag wins).
   Also `-Welaborated-enum-base` (a real Clang version/SDK-beta divergence, not
   suppressible by defining anything) disabled outright for the same reason.
6. **`externals/openal-soft/alc/backends/coreaudio.cpp` needs Objective-C++, not plain
   C++**: on this SDK, `<AudioToolbox/AudioToolbox.h>`'s umbrella header unconditionally pulls
   in `AUAudioUnit.h` -> `Foundation.h`, which contains real Objective-C declarations
   (`@class`, `NSString`, ...) with no plain-C++ guard -- a `.cpp` file simply cannot parse
   that chain. Fixed the standard way: `set_source_files_properties(... PROPERTIES LANGUAGE
   OBJCXX)` for just that one file in openal-soft's own CMakeLists.txt, which in turn needed
   `OBJCXX` added alongside the already-present `OBJC` in this project's own `project()`
   language list (proven necessary, not assumed: an existing `.mm` file elsewhere in this
   project had simply never been reached by any build attempt yet). That same Foundation/
   CoreServices/IOKit header chain also triggers *dozens* of distinct, individually-obscure
   `-Werror=undef` macros (`__clang_tapi__`, `__ppc64__`, `__BIG_ENDIAN__`, `DEBUG`, and more)
   with no practical way to enumerate and define them all -- downgraded `-Werror=undef` back
   to a warning for just this one file (via `set_source_files_properties(...
   COMPILE_OPTIONS ...)`, appended after the target's own flags so it actually wins), rather
   than continuing to whack-a-mole individual macros or weakening upstream's warning policy
   project-wide.
7. **Real fallout from the `AetherPS4` rename, only reachable once shadPS4's own CMake
   target started compiling** (as anticipated in the note above): `src/core/linker.h` had a
   forward declaration written as `class Core::FexGuestCpuBackend;` *from inside* `namespace
   Core { ... }` -- invalid syntax (a forward declaration can't carry a nested-name-specifier
   naming its own enclosing namespace) -- and a `namespace GuestCpu { ... }` forward-declaring
   `HleVeneerAllocator`/`HleGuestBridge` that was still nested inside `Core` instead of moved
   to `AetherPS4::GuestCpu` alongside the real, renamed definitions, silently declaring an
   orphaned, mismatched type. `src/core/guest_cpu/guest_callback.h` had a bare `Linker`
   reference relying on lookup through the old `Core::GuestCpu` nesting, now broken the same
   way the bare `GuestExecutionRange` references in `fex_guest_engine.cpp` were caught
   earlier. One more file, `src/core/libraries/np/np_manager.cpp`, had escaped the *original*
   rename sweep entirely: it contains a byte sequence that made plain `grep` silently treat it
   as a binary file and skip it (confirmed by re-running the same search with `grep -a`),
   so it was never in the candidate file list to begin with. A full repo audit with `grep -a`
   turned up no further instances.
8. **`src/common/crash_reporter.cpp` and `src/main.cpp`'s `SIGSYS` handler** both had their
   own independent instances of the exact Linux-`mcontext_t`-field pattern fixed earlier in
   `fex_guest_engine.cpp`/`fexcore-smoke.cpp` (`ctx->uc_mcontext.pc/sp/regs` -- Darwin's
   `uc_mcontext` is a pointer to pointer-authentication-opaque fields, not an embedded
   struct) and a `gettid()` call (no Darwin equivalent, `pthread_threadid_np` instead) --
   fixed the same way. `main.cpp`'s SIGSYS handler also read `si_syscall`/`si_arch`/
   `si_call_addr`, Linux-seccomp-populated `siginfo_t` fields with no Darwin equivalent
   (Darwin has no seccomp; SIGSYS there has no per-syscall detail to report) -- guarded with
   sentinel values, keeping the same `printf`-style format string and argument count.
9. **A pre-existing upstream mismatch in `ENABLE_BACHATA_RUNTIME`-gated Android/box64-runtime
   integration code** (unrelated to anything built or fixed this session, but only surfaced
   once shadPS4 fully linked for the first time on any platform other than Bachata's own
   Android target): `src/core/libraries/audio/bachata_audio_out.cpp` was unconditionally in
   the build's source list while its own dependency, `src/platform/bachata/
   audio_transport.cpp`, was only compiled `if (ENABLE_BACHATA_RUNTIME)` -- moved the former
   into the same conditional as the latter (confirmed safe: `audioout.cpp`'s own use of
   `BachataAudioOut` was already correctly guarded, so nothing was ever relying on it being
   unconditionally linked). Separately, `src/core/libraries/videoout/driver.cpp` -- ordinary,
   always-compiled video-out code, not Bachata-runtime-specific itself -- unconditionally
   calls `Platform::Bachata::ReportPresentedFrame()` on every frame present with no guard at
   the call site at all; rather than scattering an `#ifdef` there, gave the declaration in
   `runtime_client.h` an inline no-op fallback when `ENABLE_BACHATA_RUNTIME` is off, matching
   how `SetActiveRuntimeClient` and everything else in that header is already used elsewhere.

Per the explicit instruction for this phase, FEXCore's own identity was kept fully intact
throughout (still plain `FEXCore`/`FEXCore::CPU`/etc., untouched) -- only the two namespaces
Bachata itself carved out for the bridge layer (`Core::Fex`, `Core::GuestCpu`) were renamed,
to `AetherPS4::Fex`/`AetherPS4::GuestCpu`. `fexcore-smoke` and `fexcore-guest-harness` were
re-verified passing after every reconfigure/rebuild cycle in this phase, not just at the end.

