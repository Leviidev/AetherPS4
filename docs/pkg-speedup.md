# Speed up 7GB PKG extraction — Tier 4 + 5 + 6

Baseline measured on Poco X6 (Dark Souls Remastered, CUSA08692, 7.3 GB):
- Extract proper: **~31 MB/s** (168 s for 5.11 GB observed; AES hardware path working)
- Hidden cost NOT measured: `copyPkgToLocalCache` writes the full 7 GB to flash before extract even starts (SAF read 7 GB + flash write 7 GB). Estimated **3–8 min** on phone flash.
- Three independent bottlenecks remain: (1) the copy, (2) single-threaded serial extraction, (3) stock zlib inflate. All three addressed below.

## Goal
Cut 7GB import wall-clock from ~7–10 min → target **<2 min**, byte-identical output (`files=5902 bytes=7316600539`, unchanged).

## Tier 4 — Skip the 7GB local-cache copy (biggest win)

### Why safe
- Probe ALREADY runs on the SAF fd via `pread` and succeeds (`ImportService.kt:331-338`). Extractor is pure `pread`, zero `lseek`/`mmap`/`fopen` (verified across `core/runtime/src/main/cpp/`). So an SAF fd feeds `nativeExtract` identically to the local-cache fd.
- The copy's own doc comment (`ImportService.kt:607-609`) says it's a perf optimization ("local random reads are far faster than SAF pread"), not a correctness requirement. Direct path trades slow flash-write for possibly-slower SAF-read — net win because it eliminates the 7 GB flash write entirely.
- Fallback for non-seekable providers (cloud drives → pipe fd): keep `copyPkgToLocalCache` behind a seekability gate.

### Changes — `app/src/main/kotlin/com/bachatas4/android/service/ImportService.kt`
1. **New helper** `isOpenFdSeekable(fd: Int): Boolean` — calls `android.system.Os.lseek(fd, 0, OsConstants.SEEK_CUR)`; returns false on `ErrnoException` (ESPIPE on pipes). ~5 lines.
2. **`runPkgImport` (~line 440-470)**: open SAF fd via `contentResolver.openFileDescriptor(uri, "r")`. If `isOpenFdSeekable(pfd.fd)`:
   - Skip `copyPkgToLocalCache` + `cacheFile` entirely.
   - Pass `pfd.fd` straight to `extractWithProgress` inside `pfd.use { }` (keep open for whole extract).
   - Recompute storage math for the direct path: `required = extractBytes + margin` (no `+ packageBytes` — no cache). Log `pkg direct saf fd (skip cache)`.
   - Else: current copy path unchanged (fallback).
3. **Storage math (~line 372-404)**: branch `required` on direct-vs-copy. Direct needs no `NeedCopyConfirm` for the cache peak (peak drops to extract tree only); keep a plain insufficient-space error if `free < required`.
4. **finally (~line 590-604)**: `cacheFile?.delete()` stays defensive (null on direct path → no-op).
5. **Progress UX**: direct path emits no `ImportProgress.Copying` — goes straight to `Extracting`. `LibraryScreen.kt` copy-card branch (`isPkgCache`, ~line 534/816) simply never triggers. No Kotlin UI change needed beyond ImportService.

### Verification
- Direct path: logcat shows `pkg direct saf fd` then `nativeExtract`, NO `pkg cache copy start`. Final `extract done files=5902 bytes=7316600539` + `pkg import success bytes=7326163630`.
- Fallback: force non-seekable (hard) — covered by keeping copy path intact.

## Tier 6 — libdeflate for PFSC inflate (before Tier 5, isolated)

### Why
`DecompressPFSC` (`pkg_extractor.cpp:104-116`) calls zlib `inflate` and **discards the return value** (silent corruption risk). libdeflate is ~2× faster single-shot, MIT-licensed (`LICENSES/MIT.txt` already present), and returns a proper status. Single call site, tiny change.

### Changes
1. **New submodule**: `git submodule add https://github.com/ebiggers/libdeflate.git externals/libdeflate` (pinned tag/commit). Record in `.gitmodules`. Add `LICENSES/libdeflate-NOTICE.txt` attribution + Play notice `app/src/playstore/assets/licenses/libdeflate-NOTICE.txt`.
2. **`core/runtime/src/main/cpp/pkg/CMakeLists.txt`**: before the existing `find_library(z-lib z)`:
   ```cmake
   set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
   set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
   add_subdirectory(${CMAKE_SOURCE_DIR}/../../../../../externals/libdeflate libdeflate-build)
   target_link_libraries(bachata_pkg PRIVATE libdeflate_static)
   target_compile_definitions(bachata_pkg PRIVATE BACHATA_HAVE_LIBDEFLATE)
   ```
3. **`pkg_extractor.cpp`** — rewrite `DecompressPFSC` under `#if defined(BACHATA_HAVE_LIBDEFLATE)`:
   - Use `libdeflate_alloc_decompressor()` (cache a thread-local/singleto­n to avoid per-call alloc — but simplest: one allocor per call first, optimize later), `libdeflate_zlib_decompress(...)` (zlib-format, matches current `inflateInit`), check return `== LIBDEFLATE_SUCCESS`, `libdeflate_free_decompressor()`.
   - `#else` keep current zlib path verbatim (defensive).
   - Return `bool` (success/failure) so `extract_file` can `err = "PFSC decompress failed"; return false;` on corruption — fixes the ignored-return bug.

### Verification
- Build links `libdeflate_static` into `libbachata_pkg.so` (`nm`/`readelf` check).
- Decompressed output byte-identical to zlib path (same PKG → same `files=5902 bytes=...`).

## Tier 5 — Parallel per-file extraction (8 cores)

### Why safe (verified by exploration)
- `extract_file` reads ONLY frozen `ExtractState` fields + writes its own local `ofstream`. No shared mutation.
- `class Crypto` has NO data members (`decryptPFS` builds per-call locals) → concurrent `decryptPFS` safe.
- `pread` on shared fd is thread-safe per POSIX (doesn't mutate fd offset).
- `g_cancel` is already `std::atomic<bool>`.

### Changes — `core/runtime/src/main/cpp/pkg/pkg_extractor.cpp`
1. **Add includes**: `<thread>`, `<chrono>`, `<mutex>` (mutex already used in jni.cpp; `<atomic>` already included at line 14).
2. **`extract_file` signature (~line 520)**: change `uint64_t done_base` → `const std::atomic<uint64_t>& done_global`. Inside, progress reports `done_global.load(relaxed) + partial` instead of `done_base + partial`. Keeps per-block progress granularity (smooth bar on big files).
3. **`bachata_pkg_extract` driver loop (~line 788-812)** — replace serial loop with a worker pool:
   ```cpp
   std::atomic<uint64_t> done{0};
   std::atomic<bool> failed{false};
   std::mutex err_mu; std::string shared_err;
   std::vector<size_t> fileIdx; /* fill with PFS_FILE indices */
   std::atomic<size_t> next{0};
   unsigned n = clamp(std::thread::hardware_concurrency(), 1u, 8u, fileCount);
   auto worker = [&] {
       while (!g_cancel.load() && !failed.load()) {
           size_t i = next.fetch_add(1, std::memory_order_relaxed);
           if (i >= fileIdx.size()) break;
           const auto& t = st.fsTable[fileIdx[i]];
           std::string err;
           if (!extract_file(fd, st, t, err, progress, ctx, done, total)) {
               failed.store(true);
               std::lock_guard<std::mutex> lk(err_mu);
               if (shared_err.empty()) shared_err = err;
               break;
           }
           done.fetch_add(st.iNodeBuf[t.inode].Size, std::memory_order_relaxed);
       }
   };
   std::vector<std::thread> pool; for (unsigned k=0;k<n;++k) pool.emplace_back(worker);
   for (auto& th: pool) th.join();
   if (g_cancel.load()) return 2;
   if (failed.load()) { LOGE("parallel: %s", shared_err.c_str()); err=shared_err; return 3; }
   if (progress) progress(ctx, done.load(), total, "");
   ```
   - Progress JNI thrash control: `extract_file` keeps its existing 32-block throttle (L554 `kProgressEveryBlocks`). 8 threads × ~20 calls/s = 160 AttachCurrentThread/s — negligible (`progress_trampoline` already attaches/detaches per call, jni.cpp:27-41).
   - First-error-wins via `shared_err` + mutex. On failure, other workers finish current file then exit (`failed.load()` check at loop top).
4. **`extract_file` file-index LOGI (~line 540, "extract file #N")**: keep but use `next.load()` ordering is racy — acceptable (log lines may interleave; LOGI is thread-safe). Drop the `file_index % 25` gate to reduce noise, or keep — minor.

### Verification
- Re-import Dark Souls: `extract done files=5902 bytes=7316600539` (byte-identical).
- Throughput: expect aggregate decrypt+inflate across 8 cores. If SAF `pread` serializes (Tier 4 direct), gain is smaller but still > serial.
- Stress: import twice back-to-back, compare `files`/`bytes` each time (race detection by repetition).

## Combined verification (per AGENTS.md)
1. Runtime assets unchanged (native-only change): no `runtime/scripts/build-runtime-debian.sh` rebuild needed. APK native lib rebuilds via gradle.
2. `cd android/BachataS4 && ./gradlew :core:runtime:clean test lintDebug assembleDebug` (clean to flush stale .class paths; tier 3 needed this).
3. `unzip -l app/build/outputs/apk/debug/app-debug.apk | grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'` — both must be present.
4. `unzip -l ... | grep libbachata_pkg` — present for arm64-v8a.
5. Install on Poco X6; delete prior `CUSA08692`; re-import Dark Souls `.pkg`.
6. Confirm: `extract done files=5902 bytes=7316600539`, `pkg import success id=CUSA08692 bytes=7326163630`, no `pkg cache copy start` (Tier 4 working).
7. Measure extract throughput from logcat timeline; compare to 31 MB/s baseline.
8. (Optional) checksum a large extracted file (e.g. `eboot.bin` or a `.tpfbdt`) against prior extraction for byte-identical proof.

## Risk / scope
- **Tier 4**: SAF `pread` throughput untested on this device/provider — mitigated by automatic copy fallback on non-seekable fds. If SAF pread is slow AND Tier 5 doesn't compensate, net could be smaller than hoped; copy path remains as escape hatch.
- **Tier 5**: threading races — mitigated by read-only-state proof + atomic counters + repeated-import verification. No locks on hot path (only error path uses mutex).
- **Tier 6**: new git submodule (supply-chain) — pinned commit, MIT, attribution recorded. zlib fallback kept under `#else`.
- No Kotlin/DI/manifest/runtime-zip changes except ImportService storage-math branch.
- Does NOT touch the separate Compose-layout crashes.

## Implementation order
Tier 4 → Tier 6 → Tier 5, measuring after each so each tier's contribution is visible in logcat. Commit after each verified tier.
