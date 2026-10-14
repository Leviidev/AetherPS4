# FEXCore -- Apple Silicon / arm64-apple-darwin port

This is a snapshot of [FEXCore](https://github.com/FEX-Emu/FEX) (the x86-64 -> ARM64
dynamic recompiler at the heart of [FEX-Emu](https://github.com/FEX-Emu/FEX)), pinned at
upstream commit `f2b679f6028ce1c38875233aecfcf5d3f8ebecec` and patched to build and run
**natively on macOS ARM64 (Apple Silicon)**, with no Rosetta 2, no x86_64 binary, and no
Linux/Android runtime involved anywhere.

It was extracted from [Bachata-S4](https://github.com/JICA98/Bachata-S4) (a shadPS4 fork
that embeds FEXCore as its guest-CPU backend on Android/ARM64), during the work to bring
that same architecture to native macOS. It's kept here as a standalone, reusable copy for
any future project that needs a working ARM64-host x86-64 JIT on Darwin, independent of
Bachata-S4's own build system and release cycle.

## What's verified working

Two standalone probes exercise this exact tree end-to-end on real Apple Silicon hardware:

- **`fexcore-smoke`**: guest x86-64 arithmetic, floating point, multi-threading, TLS,
  host<->guest callbacks, and self-modifying-code cache invalidation, all executed through
  FEXCore's JIT.
- **`fexcore-guest-harness`**: the same, plus a guest-CPU-backend abstraction layer and an
  HLE (high-level emulation) veneer/bridge mechanism for host<->guest syscall-style calls.

Both build as genuine `Mach-O 64-bit executable arm64` with zero x86_64 dependencies
(verified with `file`/`lipo -info`/`otool -L`), and both pass every check on a fresh run.
The probe sources themselves aren't included in this snapshot (they're Bachata-S4-specific
harness code, not part of FEXCore) -- see Bachata-S4's `runtime/probes/` and `PORTING.md`
for the full probes and the complete narrative of every fix below.

## What changed from upstream

Everything is a single commit on top of the pinned upstream commit
(`f2b679f6028ce1c38875233aecfcf5d3f8ebecec`), titled "Port FEXCore to arm64-apple-darwin
(Apple Silicon)". In brief, in the order they were found:

1. **Build system**: allow `Darwin` as a recognized `CMAKE_SYSTEM_NAME`; a
   `BUILD_FEXCORE_ONLY` CMake option (added by Bachata-S4, not upstream FEX) to build just
   `FEXCore` + embeddable smoke/harness executables without the full Linux-application/
   syscall-emulation machinery.
2. **Missing/renamed Darwin libc surface**: `<malloc.h>` -> `<malloc/malloc.h>`;
   `memalign()`/`valloc()` (absent on Darwin) reimplemented via `posix_memalign()`;
   `malloc_usable_size()` -> `malloc_size()`; `linux/limits.h` -> `<limits.h>` for
   `PATH_MAX`; `sendfile()`'s Darwin semantics (socket-only, not file-to-file) worked
   around with a manual copy loop; `sincos()` -> Darwin's `__sincos`; `prctl()` stubbed out
   (`ENOSYS`); raw syscall fallbacks (`SYS_futex`, `SYS_rt_sigprocmask`, `getcpu`/`gettid`)
   replaced with Darwin's public `os_sync_wait_on_address`/`pthread_sigmask`/
   `pthread_threadid_np` equivalents; `MADV_HUGEPAGE`/`NOHUGEPAGE`/`DONTDUMP` no-op'd (no
   Darwin equivalent, pure performance/debugging hints).
3. **CPU feature detection**: Darwin's XNU kernel does not emulate EL0 reads of the
   `ID_AA64*_EL1`/`MIDR_EL1` system registers the way Linux's kernel does (confirmed via
   standalone crash repros) -- replaced with `sysctlbyname("hw.optional.arm.FEAT_*", ...)`
   queries. `CTR_EL0` also SIGILLs on this XNU kernel (unlike the architecturally-similar
   `DCZID_EL0`, which doesn't) -- replaced with `sysctlbyname("hw.cachelinesize", ...)`.
4. **JIT memory model (the deep part)**: Darwin requires `MAP_JIT` for any executable
   anonymous mapping, and `pthread_jit_write_protect_np(0/1)` toggles a *global per-thread*
   W^X state (not per-mapping) around writes to `MAP_JIT` pages -- three distinct call
   sites needed this toggle (JIT dispatcher bootstrap, per-block compilation, and runtime
   inline-call-site patching). Darwin's `mremap`-equivalent (`mach_vm_remap`) categorically
   refuses any source mapping containing `PROT_EXEC`, and separately refuses
   `MAP_FIXED`+`MAP_JIT` together and any `mprotect()` on a sub-range of an existing
   `MAP_JIT` mapping -- all confirmed empirically with standalone repros before touching
   any FEXCore source, per FEXCore's own code-cache and code-buffer allocation semantics.
5. **Host-vs-guest page size**: Apple Silicon's host page size is 16KB, not the 4KB every
   affected call site assumed (silently conflated with x86-64's fixed *guest* page size,
   which happens to equal Linux's usual host page size too) -- fixed at each site to use
   the real `sysconf(_SC_PAGESIZE)`.
6. **Signal-context register access**: Darwin's `ucontext_t`/`mcontext_t` layout differs
   structurally from Linux's (`uc_mcontext` is a pointer, not an embedded struct; pc/sp/fp/
   lr are pointer-authentication-opaque fields requiring accessor macros from
   `<mach/arm/thread_status.h>` rather than direct field access).

Full technical narrative, including the exact empirical verification for every
correctness-sensitive fix above, is in Bachata-S4's `PORTING.md`.

## Using this elsewhere

This tree builds with `BUILD_FEXCORE_ONLY=ON` via CMake + Ninja, producing a static
`FEXCore` library plus whatever smoke-test executable you point
`FEXCORE_SMOKE_SOURCE` at. See Bachata-S4's `runtime/build/fexcore-macos-build`
configure invocation for the full flag list this was last built and verified with.
