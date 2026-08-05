# PKG Install State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evolve the existing PKG/folder import path into an explicit transactional install state machine so valid packages install and launch without manual folder intervention, and partial installs never appear as completed games.

**Architecture:** Approach A from the spec: rename/extend `ImportProgress` to formal install states, persist `job.json` under `files/games/.jobs/`, write `install.manifest` before atomic promote, register games only through one verify+DB path, and gate launch on manifest + eboot. `ImportService` remains the driver; no parallel installer module.

**Tech Stack:** Kotlin, JUnit4, Android Service/SAF, existing `PkgExtractor` JNI, Room `GameDao`, Jetpack Compose library UI.

**Spec:** `docs/superpowers/specs/2026-08-06-pkg-install-state-machine-design.md`  
**Prior design (crypto/extract host unchanged):** `docs/superpowers/specs/2026-07-23-pkg-import-design.md`

## Global Constraints

- App-owned install root only: `context.filesDir/games/`
- DB insert only after verify + atomic promote + `install.manifest` with `status=INSTALLED`
- Launch only when `canLaunch` is true (DB + manifest + eboot)
- Typed failures use `InstallErrorCode` on `ImportProgress.Failed`
- Device acceptance: **OnePlus Pad 2 (`OPD2403`) only** — never claim pass on other devices
- Prefer TDD: failing test → implement → pass → commit per task
- Do not expand scope into multi-job queue or mid-file extract resume

## File map

| Path | Role |
|------|------|
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallErrorCode.kt` | Typed install errors |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ImportManager.kt` | State machine + busy slot |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallManifest.kt` | Read/write/verify install.manifest |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallJobStore.kt` | Persist job.json |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallCleanup.kt` | Cleanup + startup reconcile helpers |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/GameInstallVerifier.kt` | Shared canLaunch / tree verify |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ContentImporter.kt` | Staging verify + manifest + atomic move |
| `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/GameRepository.kt` | Sole register path; replace orphan sync |
| `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/ImportService.kt` | Drive states for PKG + folder |
| `android/BachataS4/feature/library/.../LibraryScreen.kt` | UI for new states / errors |
| `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/DirectGameLaunchRequest.kt` | Launch gate + path bugfix |
| Matching `*/src/test/**` | Unit tests |

---

### Task 1: `InstallErrorCode` + formal `ImportProgress` states

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallErrorCode.kt`
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ImportManager.kt`
- Modify: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ImportManagerTest.kt`
- Modify (compile fix only this task if needed for Success/Finalizing renames): call sites listed in Task 7–8 may still use old names until those tasks; **in this task** keep temporary `@Deprecated` typealiases **or** update all references in the same commit — prefer **update all sealed-type references in one commit** so the project compiles.

**Interfaces:**
- Produces: `enum class InstallErrorCode` with values from the spec
- Produces: new `ImportProgress` variants (see code below)
- Produces: `ImportManager.isBusy` includes all non-terminal install states

- [ ] **Step 1: Write failing tests for new states and busy rules**

Replace/extend `ImportManagerTest.kt`:

```kotlin
package com.bachatas4.android.data

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ImportManagerTest {
    @After
    fun tearDown() {
        ImportManager.reset()
    }

    @Test
    fun tryBeginImportClaimsSlotWithSelected() {
        assertTrue(ImportManager.tryBeginImport())
        assertTrue(ImportManager.progress.value is ImportProgress.Selected)
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
    }

    @Test
    fun validatingThroughRegisteringAreBusy() {
        val busyStates = listOf(
            ImportProgress.Selected("content://x", ImportManager.MODE_PKG),
            ImportProgress.Validating("content://x", ImportManager.MODE_PKG),
            ImportProgress.ReadingMetadata("pkg", null),
            ImportProgress.CheckingStorage("id", 1L, 2L),
            ImportProgress.Extracting(0L, 100L, "f", "t"),
            ImportProgress.Copying(0L, 100L, "f", "t"),
            ImportProgress.Verifying("t"),
            ImportProgress.Registering("t"),
            ImportProgress.NeedPasscode("cid", "hint"),
            ImportProgress.NeedCopyConfirm("cid", "hint", 1, 1, 2, 3),
        )
        for (state in busyStates) {
            ImportManager.update(state)
            assertTrue("expected busy: $state", ImportManager.isBusy(state))
            assertFalse(ImportManager.tryBeginImport())
            ImportManager.reset()
        }
    }

    @Test
    fun installedAndFailedAreNotBusy() {
        ImportManager.update(ImportProgress.Installed("id", "Title"))
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
        ImportManager.update(
            ImportProgress.Failed(InstallErrorCode.MALFORMED_PACKAGE, "bad header"),
        )
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
    }

    @Test
    fun failedCarriesTypedCode() {
        ImportManager.update(
            ImportProgress.Failed(InstallErrorCode.INSUFFICIENT_STORAGE, "need 10 GiB"),
        )
        val failed = ImportManager.progress.value as ImportProgress.Failed
        assertEquals(InstallErrorCode.INSUFFICIENT_STORAGE, failed.code)
        assertEquals("need 10 GiB", failed.message)
    }
}
```

- [ ] **Step 2: Run tests — expect compile/fail**

```bash
cd android/BachataS4
./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.ImportManagerTest'
```

Expected: FAIL (types missing / `Preparing` still used / `tryBeginImport` still sets `Preparing`).

- [ ] **Step 3: Implement `InstallErrorCode` + `ImportProgress` + `ImportManager`**

`InstallErrorCode.kt`:

```kotlin
package com.bachatas4.android.data

enum class InstallErrorCode {
    SOURCE_INACCESSIBLE,
    UNSUPPORTED_TYPE,
    BAD_HEADER,
    NO_TITLE_ID,
    INSUFFICIENT_STORAGE,
    DEST_NOT_WRITABLE,
    PARTIAL_EXISTS,
    BASE_MISSING,
    UNSUPPORTED_ENCRYPTION,
    MALFORMED_PACKAGE,
    VERIFY_FAILED,
    ALREADY_INSTALLED,
    INTERRUPTED,
    PERMISSION_LOST,
    CANCELLED,
    UNKNOWN,
}
```

Replace sealed interface body in `ImportManager.kt` with:

```kotlin
sealed interface ImportProgress {
    data object Idle : ImportProgress

    data class Selected(val sourceUri: String, val mode: String) : ImportProgress

    data class Validating(val sourceUri: String, val mode: String) : ImportProgress

    data class ReadingMetadata(
        val displayName: String,
        val contentId: String?,
    ) : ImportProgress

    data class CheckingStorage(
        val contentId: String,
        val requiredBytes: Long,
        val freeBytes: Long,
    ) : ImportProgress

    data class Extracting(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Copying(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Verifying(val title: String) : ImportProgress

    data class Registering(val title: String) : ImportProgress

    data class NeedPasscode(val contentId: String, val titleHint: String?) : ImportProgress

    data class NeedCopyConfirm(
        val contentId: String,
        val titleHint: String?,
        val packageBytes: Long,
        val extractBytes: Long,
        val requiredBytes: Long,
        val freeBytes: Long,
    ) : ImportProgress

    data class Installed(val gameId: String, val title: String) : ImportProgress

    data class Failed(val code: InstallErrorCode, val message: String) : ImportProgress
}
```

Update `isBusy` to treat every non-`Idle` / non-`Installed` / non-`Failed` state as busy (including `Selected`, `Validating`, `ReadingMetadata`, `CheckingStorage`, `Verifying`, `Registering`, gates, extract/copy).

Change `tryBeginImport()` to CAS into:

```kotlin
ImportProgress.Selected(sourceUri = "", mode = "")
```

Service will immediately overwrite with real URI/mode on `ACTION_IMPORT`. Alternatively add:

```kotlin
fun tryBeginImport(sourceUri: String, mode: String): Boolean
```

Prefer the two-arg form and update `ImportService` call site in Task 7; for this task, if service still calls zero-arg, keep zero-arg setting `Selected("", "")` and service updates next.

Update every `ImportProgress.Success` → `Installed`, `Finalizing` → `Registering`, `Failed("...")` → `Failed(code, "...")`, remove `Preparing`/`Scanning` usages across modules so `:core:data:testDebugUnitTest` and app compile. Minimal mapping for old Scanning: use `ReadingMetadata(folderName, null)`.

- [ ] **Step 4: Run unit tests**

```bash
cd android/BachataS4
./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.ImportManagerTest'
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallErrorCode.kt \
  android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ImportManager.kt \
  android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ImportManagerTest.kt \
  # plus any compile-fix renames required
git commit -m "feat(install): formal ImportProgress state machine"
```

---

### Task 2: `InstallManifest` + `GameInstallVerifier`

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallManifest.kt`
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/GameInstallVerifier.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/InstallManifestTest.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/GameInstallVerifierTest.kt`

**Interfaces:**
- Consumes: none beyond `java.io.File` + org.json or kotlinx.serialization already on classpath — **use `org.json.JSONObject`** if available on Android unit tests, else minimal hand-written JSON for the few fields (prefer `JSONObject` as used elsewhere if present; else simple string format matching spec fields).
- Produces:

```kotlin
data class InstallManifest(
    val version: Int = 1,
    val status: String, // "INSTALLED"
    val gameId: String,
    val contentId: String?,
    val mode: String,
    val sourceUri: String,
    val installedAtMs: Long,
    val requiredFiles: List<String>,
    val bytesTotal: Long,
)

object InstallManifestIo {
    const val FILE_NAME = "install.manifest"
    const val STATUS_INSTALLED = "INSTALLED"
    fun write(dir: File, manifest: InstallManifest)
    fun read(dir: File): InstallManifest?
}

object GameInstallVerifier {
    fun requiredFilesPresent(gameDir: File): Boolean
    fun canLaunch(filesDir: File, relativePath: String): Boolean
    fun verifyTreeForRegistration(gameDir: File, expectedGameId: String?): VerifyResult
    sealed class VerifyResult {
        data class Ok(val bytesTotal: Long) : VerifyResult
        data class Fail(val code: InstallErrorCode, val message: String) : VerifyResult
    }
}
```

- [ ] **Step 1: Failing tests**

```kotlin
// InstallManifestTest.kt
@Test
fun writeAndReadRoundTrip() {
    val dir = createTempDir()
    val m = InstallManifest(
        status = InstallManifestIo.STATUS_INSTALLED,
        gameId = "CUSA00000",
        contentId = "EP0001-CUSA00000_00-TEST",
        mode = "pkg",
        sourceUri = "content://pkg",
        installedAtMs = 123L,
        requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
        bytesTotal = 99L,
    )
    InstallManifestIo.write(dir, m)
    val read = InstallManifestIo.read(dir)!!
    assertEquals("CUSA00000", read.gameId)
    assertEquals(InstallManifestIo.STATUS_INSTALLED, read.status)
    assertEquals(99L, read.bytesTotal)
}

// GameInstallVerifierTest.kt
@Test
fun canLaunchRequiresManifestAndEboot() {
    val filesDir = createTempDir()
    val game = File(filesDir, "games/CUSA00000").apply { mkdirs() }
    assertFalse(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
    File(game, "eboot.bin").writeBytes(byteArrayOf(1))
    assertFalse(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
    InstallManifestIo.write(
        game,
        InstallManifest(
            status = "INSTALLED",
            gameId = "CUSA00000",
            contentId = null,
            mode = "folder",
            sourceUri = "",
            installedAtMs = 1L,
            requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
            bytesTotal = 1L,
        ),
    )
    // still false without param.sfo if verifier requires it for canLaunch —
    // Spec canLaunch: manifest INSTALLED + eboot. param.sfo required for registration verify only.
    assertTrue(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
}

@Test
fun verifyTreeFailsWithoutEboot() {
    val game = createTempDir()
    File(game, "sce_sys").mkdirs()
    File(game, "sce_sys/param.sfo").writeBytes(byteArrayOf(0)) // invalid sfo ok for this assert path
    val result = GameInstallVerifier.verifyTreeForRegistration(game, null)
    assertTrue(result is GameInstallVerifier.VerifyResult.Fail)
    assertEquals(
        InstallErrorCode.VERIFY_FAILED,
        (result as GameInstallVerifier.VerifyResult.Fail).code,
    )
}
```

Use a minimal valid SFO fixture from `ParamSfoReaderTest` if needed for happy-path verify tests.

- [ ] **Step 2: Run — expect fail**

```bash
cd android/BachataS4
./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.InstallManifestTest' \
  --tests 'com.bachatas4.android.data.GameInstallVerifierTest'
```

- [ ] **Step 3: Implement**

`InstallManifestIo.write`: write `install.manifest.tmp` then `Files.move(..., ATOMIC_MOVE)` or delete+rename to `install.manifest`.

`GameInstallVerifier.canLaunch`:

```kotlin
fun canLaunch(filesDir: File, relativePath: String): Boolean {
    val root = File(filesDir, relativePath).canonicalFile
    val games = File(filesDir, "games").canonicalFile
    if (!root.toPath().startsWith(games.toPath())) return false
    if (!root.isDirectory) return false
    val manifest = InstallManifestIo.read(root) ?: return false
    if (manifest.status != InstallManifestIo.STATUS_INSTALLED) return false
    val eboot = File(root, "eboot.bin")
    return eboot.isFile && eboot.length() > 0L
}
```

`verifyTreeForRegistration`: require eboot non-empty, `sce_sys/param.sfo` file exists (parse optional for id match when `expectedGameId` non-null using `ParamSfoReader`), compute `bytesTotal` via walk, return Ok/Fail.

- [ ] **Step 4: Tests pass**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): install.manifest and game verify helpers"
```

---

### Task 3: `InstallJobStore`

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallJobStore.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/InstallJobStoreTest.kt`

**Interfaces:**

```kotlin
data class InstallJob(
    val version: Int = 1,
    val jobId: String,
    val state: String, // SELECTED, VALIDATING, ... INSTALLED, FAILED
    val mode: String,
    val sourceUri: String,
    val uriPersistable: Boolean = false,
    val displayName: String? = null,
    val contentId: String? = null,
    val titleId: String? = null,
    val stagingDir: String? = null, // relative "games/.import-<id>"
    val cachePath: String? = null,
    val packageBytes: Long = 0L,
    val extractBytes: Long = 0L,
    val requiredBytes: Long = 0L,
    val createdAtMs: Long,
    val updatedAtMs: Long,
    val lastErrorCode: String? = null,
    val lastErrorMessage: String? = null,
)

class InstallJobStore(private val filesDir: File) {
    fun jobsRoot(): File // filesDir/games/.jobs
    fun create(job: InstallJob)
    fun update(job: InstallJob)
    fun read(jobId: String): InstallJob?
    fun list(): List<InstallJob>
    fun delete(jobId: String)
}
```

State strings must match spec names: `SELECTED`, `VALIDATING`, `READING_METADATA`, `CHECKING_STORAGE`, `EXTRACTING`, `VERIFYING`, `REGISTERING`, `INSTALLED`, `FAILED` (plus optional `NEED_PASSCODE`, `NEED_COPY_CONFIRM`, `COPYING` for folder).

- [ ] **Step 1: Failing test** — create, update state, list, delete; atomic file exists as `job.json`.

- [ ] **Step 2: Run fail**

- [ ] **Step 3: Implement** with `job.json.tmp` → rename under `filesDir/games/.jobs/<jobId>/`.

- [ ] **Step 4: Pass**

```bash
./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.InstallJobStoreTest'
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): persist install job.json"
```

---

### Task 4: `InstallCleanup` + reconcile helpers

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/InstallCleanup.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/InstallCleanupTest.kt`

**Interfaces:**

```kotlin
class InstallCleanup(
    private val filesDir: File,
    private val jobStore: InstallJobStore,
) {
    fun cleanupJob(jobId: String)
    /** Delete orphan .import-* when import idle; drop terminal/stale jobs. */
    fun cleanupStaleArtifacts(importBusy: Boolean)
    /**
     * Returns at most one job that is safe to resume, after marking others failed/cleaned.
     * Does not start work — caller (ImportService / Library) decides.
     */
    fun findResumableJob(): InstallJob?
}
```

- [ ] **Step 1: Tests**

```kotlin
@Test
fun cleanupJobRemovesStagingCacheAndJobDir() {
    val filesDir = createTempDir()
    val store = InstallJobStore(filesDir)
    val jobId = "j1"
    val staging = File(filesDir, "games/.import-$jobId").apply { mkdirs() }
    File(staging, "x").writeText("x")
    val cache = File(filesDir, "pkg-cache/$jobId.pkg").apply {
        parentFile!!.mkdirs()
        writeText("pkg")
    }
    store.create(sampleJob(jobId, stagingDir = "games/.import-$jobId", cachePath = "pkg-cache/$jobId.pkg"))
    InstallCleanup(filesDir, store).cleanupJob(jobId)
    assertFalse(staging.exists())
    assertFalse(cache.exists())
    assertNull(store.read(jobId))
}

@Test
fun cleanupStaleArtifactsRemovesImportDirsWhenIdle() {
    val filesDir = createTempDir()
    val orphan = File(filesDir, "games/.import-orphan").apply { mkdirs() }
    InstallCleanup(filesDir, InstallJobStore(filesDir)).cleanupStaleArtifacts(importBusy = false)
    assertFalse(orphan.exists())
}

@Test
fun cleanupStaleArtifactsSkipsImportDirsWhenBusy() {
    val filesDir = createTempDir()
    val orphan = File(filesDir, "games/.import-active").apply { mkdirs() }
    InstallCleanup(filesDir, InstallJobStore(filesDir)).cleanupStaleArtifacts(importBusy = true)
    assertTrue(orphan.exists())
}
```

- [ ] **Step 2–4: Implement + pass**

Resume selection: prefer single job in `EXTRACTING`/`COPYING`/`VERIFYING`/`REGISTERING` with existing cache or staging; others `cleanupJob`. Do not auto-delete final `games/<id>` trees.

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): cleanup and reconcile install artifacts"
```

---

### Task 5: Transactional `ContentImporter` + manifest on promote

**Files:**
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ContentImporter.kt`
- Modify: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ContentImporterTest.kt`

**Interfaces:**
- Extend `finalizeStagingTree` to accept optional contentId/mode (or read from request): write `install.manifest` into staging after verify, then atomic move.
- Extend `importGameTree` similarly so folder path gets the same manifest.
- Produces destination always containing `install.manifest` with `STATUS_INSTALLED`.

- [ ] **Step 1: Failing tests**

```kotlin
@Test
fun finalizeStagingTreeWritesInstallManifest() = runTest {
    val gamesDir = File(tempRoot, "games").apply { mkdirs() }
    val staging = File(gamesDir, ".import-pkg").apply { mkdirs() }
    // copy minimal eboot + param.sfo fixture into staging (reuse existing test helpers)
    writeMinimalGameTree(staging)
    val result = importer.finalizeStagingTree(
        ContentImportRequest(
            id = "CUSA00000",
            title = "Test",
            sourceUri = "content://pkg",
            subtitle = null,
            detail = null,
        ),
        staging,
        mode = "pkg",
        contentId = "EP0001-CUSA00000_00-TEST",
    )
    val dest = File(gamesDir, "CUSA00000")
    assertTrue(dest.isDirectory)
    val manifest = InstallManifestIo.read(dest)!!
    assertEquals("INSTALLED", manifest.status)
    assertEquals("pkg", manifest.mode)
    assertTrue(File(dest, "eboot.bin").isFile)
}

@Test
fun finalizeStagingTreeRejectsMissingEbootWithoutLeavingDestination() = runTest {
    // existing test + assert no install.manifest on gamesDir children
}
```

Add `mode` / `contentId` parameters with defaults to limit call-site churn:

```kotlin
suspend fun finalizeStagingTree(
    request: ContentImportRequest,
    stagingDir: File,
    mode: String = ImportManager.MODE_FOLDER,
    contentId: String? = null,
): ContentImportResult
```

- [ ] **Step 2: Run fail**

- [ ] **Step 3: Implement**

Before `moveAtomically`:

```kotlin
val verify = GameInstallVerifier.verifyTreeForRegistration(staging, request.id)
if (verify is GameInstallVerifier.VerifyResult.Fail) {
    throw ContentImportException(RuntimeErrorCode.CONTENT_INVALID, verify.message)
}
val bytes = (verify as GameInstallVerifier.VerifyResult.Ok).bytesTotal
InstallManifestIo.write(
    staging,
    InstallManifest(
        status = InstallManifestIo.STATUS_INSTALLED,
        gameId = request.id,
        contentId = contentId,
        mode = mode,
        sourceUri = request.sourceUri,
        installedAtMs = System.currentTimeMillis(),
        requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
        bytesTotal = bytes,
    ),
)
moveAtomically(staging, destination)
```

Same for `importGameTree` after copy into staging, before move.

- [ ] **Step 4: Full ContentImporter tests pass**

```bash
./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.ContentImporterTest'
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): write install.manifest on transactional promote"
```

---

### Task 6: Unified registration + fix orphan sync

**Files:**
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/GameRepository.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/GameRepositoryInstallTest.kt` (use in-memory fakes: fake GameDao list + temp filesDir — if GameDao is Room-only, test pure helper extracted as `LibrarySync` object to avoid Room instrumentation)

**Interfaces:**

```kotlin
// On GameRepository
suspend fun addImportedGame(...) // keep name; document: only call after promote with manifest

suspend fun syncLibrary() // replaces syncOrphanedFolders

// Prefer extract pure logic for unit tests:
object LibrarySync {
    data class Action(val id: String, val title: String, val relativePath: String, /* ... */)
    fun planSync(
        gamesRoot: File,
        dbIds: Set<String>,
        importBusy: Boolean,
    ): SyncPlan // inserts, skips, cleanups
}
```

- [ ] **Step 1: Failing tests for `LibrarySync.planSync`**

```kotlin
@Test
fun incompleteFolderNotScheduledForInsert() {
    val root = createTempDir()
    val games = File(root, "games").apply { mkdirs() }
    File(games, "HALF").mkdirs() // no eboot
    val plan = LibrarySync.planSync(games, dbIds = emptySet(), importBusy = false)
    assertTrue(plan.inserts.isEmpty())
}

@Test
fun completeTreeWithoutManifestHealsAndInserts() {
    val root = createTempDir()
    val games = File(root, "games").apply { mkdirs() }
    val g = File(games, "CUSA00000").apply { mkdirs() }
    writeMinimalGameTree(g) // eboot + param.sfo
    val plan = LibrarySync.planSync(games, emptySet(), false)
    assertEquals(1, plan.inserts.size)
    assertEquals("CUSA00000", plan.inserts[0].id)
    // after applyHeal, manifest exists
}

@Test
fun orphanImportDirScheduledCleanupWhenIdle() {
    val games = File(createTempDir(), "games").apply { mkdirs() }
    File(games, ".import-x").mkdirs()
    val plan = LibrarySync.planSync(games, emptySet(), importBusy = false)
    assertTrue(plan.stagingDirsToDelete.any { it.endsWith(".import-x") })
}
```

- [ ] **Step 2: Implement `LibrarySync` + `GameRepository.syncLibrary()` calling it; deprecate/remove body of `syncOrphanedFolders` by making it delegate to `syncLibrary()` for one release, or replace call sites.

- [ ] **Step 3: Update `LibraryScreen` call** `syncOrphanedFolders` → `syncLibrary` (can be this task).

- [ ] **Step 4: Tests pass**

- [ ] **Step 5: Commit**

```bash
git commit -m "fix(install): register only verified library trees"
```

---

### Task 7: `ImportService` full state machine (PKG + folder)

**Files:**
- Modify: `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/ImportService.kt`
- Optionally create pure validator helper in `core/data`: `InstallValidator.kt` for pre-extract checks testable without Android Service.

**Interfaces:**
- Consumes: `InstallJobStore`, `InstallCleanup`, `GameInstallVerifier`, `InstallManifestIo`, `ContentImporter`, `PkgExtractor`, `ImportManager`
- Emits progress in order: Selected → Validating → ReadingMetadata → CheckingStorage → (gates) → Extracting/Copying → Verifying → Registering → Installed | Failed(code)

- [ ] **Step 1: Unit-test pure validation mapping** in `core/data` `InstallValidatorTest`:

```kotlin
object InstallValidator {
    fun mapProbeFailure(status: /* string or enum */): InstallErrorCode
    fun checkStorage(required: Long, free: Long): InstallErrorCode?
    fun checkDestination(dest: File, canLaunch: Boolean): InstallErrorCode?
}

@Test
fun insufficientStorageWhenFreeBelowRequired() {
    assertEquals(
        InstallErrorCode.INSUFFICIENT_STORAGE,
        InstallValidator.checkStorage(required = 100, free = 10),
    )
}

@Test
fun alreadyInstalledWhenDestLaunchable() {
    // temp dest with manifest+eboot
    assertEquals(
        InstallErrorCode.ALREADY_INSTALLED,
        InstallValidator.checkDestination(dest, canLaunch = true),
    )
}
```

- [ ] **Step 2: Implement `InstallValidator`**

- [ ] **Step 3: Refactor `runPkgImport`**

Pseudocode to implement in full Kotlin:

```kotlin
private suspend fun runPkgImport(uriString: String) {
    val jobStore = InstallJobStore(filesDir)
    val cleanup = InstallCleanup(filesDir, jobStore)
    val jobId = UUID.randomUUID().toString()
    var staging: File? = null
    var cacheFile: File? = null
    var completed = false
    try {
        ImportManager.update(ImportProgress.Selected(uriString, ImportManager.MODE_PKG))
        jobStore.create(InstallJob(
            jobId = jobId,
            state = "SELECTED",
            mode = ImportManager.MODE_PKG,
            sourceUri = uriString,
            createdAtMs = now(),
            updatedAtMs = now(),
        ))

        ImportManager.update(ImportProgress.Validating(uriString, ImportManager.MODE_PKG))
        jobStore.update(...) // state VALIDATING
        val uri = Uri.parse(uriString)
        val persistable = runCatching {
            contentResolver.takePersistableUriPermission(uri, FLAG_GRANT_READ_URI_PERMISSION)
            true
        }.getOrDefault(false)

        // open + probe
        ImportManager.update(ImportProgress.ReadingMetadata(...))
        val probe = ... nativeProbe ...
        if (probe.status == ERROR) error with MALFORMED_PACKAGE / BAD_HEADER from message

        ImportManager.update(ImportProgress.CheckingStorage(...))
        // free space; NeedCopyConfirm gate (keep)
        // destination checks via InstallValidator

        staging = File(filesDir, "games/.import-$jobId")
        // cache + extract as today, progress Extracting
        // NeedPasscode as today

        ImportManager.update(ImportProgress.Verifying(title))
        // finalizeStagingTree writes manifest + move
        ImportManager.update(ImportProgress.Registering(title))
        gameRepository.addImportedGame(...)
        jobStore.update(state = INSTALLED) ; jobStore.delete(jobId) // or keep terminal then cleanup
        ImportManager.update(ImportProgress.Installed(id, title))
        completed = true
    } catch (t: Throwable) {
        handleFailureTyped(t) // map to InstallErrorCode
    } finally {
        if (!completed) cleanup.cleanupJob(jobId) // careful: don't delete if promoted
        // only cleanup staging if still under .import-
    }
}
```

Folder path: same states with `Copying` instead of `Extracting`; validate source has eboot + param.sfo before copy.

`handleFailure`: always `ImportProgress.Failed(code, message)` never string-only.

Cancel → `Failed(CANCELLED, "cancelled")` after cleanup.

- [ ] **Step 4: Compile app module**

```bash
cd android/BachataS4
./gradlew :app:compileFdroidDebugKotlin
```

Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): ImportService transactional state machine"
```

---

### Task 8: Library UI + launch gate

**Files:**
- Modify: `android/BachataS4/feature/library/src/main/kotlin/com/bachatas4/android/feature/library/LibraryScreen.kt`
- Modify: `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/DirectGameLaunchRequest.kt`
- Modify: `android/BachataS4/app/src/test/kotlin/com/bachatas4/android/DirectGameLaunchRequestTest.kt`
- Modify launch path in `MainActivity` / nav if it launches without eboot check — ensure `GameInstallVerifier.canLaunch` used before start

**DirectGameLaunchRequest bug:** current source uses `File(gameRoot, gameId)` before `gameRoot` is assigned — must be `File(gamesRoot, gameId)`. Fix while adding manifest check.

- [ ] **Step 1: Failing tests**

```kotlin
@Test
fun resolveRejectsWhenInstallManifestMissing() {
    val filesDir = createTempDir()
    val game = File(filesDir, "games/CUSA00000").apply { mkdirs() }
    File(game, "eboot.bin").writeBytes(byteArrayOf(1))
    val r = DirectGameLaunchRequest.resolve(filesDir, "CUSA00000")
    assertTrue(r is DirectGameLaunchRequest.Resolution.Rejected)
}

@Test
fun resolveReadyWhenManifestAndEbootPresent() {
    val filesDir = createTempDir()
    val game = File(filesDir, "games/CUSA00000").apply { mkdirs() }
    File(game, "eboot.bin").writeBytes(byteArrayOf(1))
    InstallManifestIo.write(game, /* INSTALLED manifest */)
    val r = DirectGameLaunchRequest.resolve(filesDir, "CUSA00000")
    assertEquals(DirectGameLaunchRequest.Resolution.Ready("CUSA00000"), r)
}
```

Note: `core:data` types from `app` tests — app already depends on data; if `InstallManifestIo` not visible, duplicate minimal check in `DirectGameLaunchRequest` using `GameInstallVerifier.canLaunch`.

- [ ] **Step 2: Implement resolve with `GameInstallVerifier.canLaunch`**

```kotlin
gameRoot = File(gamesRoot, gameId).canonicalFile
// ...
if (!GameInstallVerifier.canLaunch(filesDir, "games/$gameId")) {
    return Resolution.Rejected("Imported game $gameId is not fully installed")
}
return Resolution.Ready(gameId)
```

- [ ] **Step 3: LibraryScreen `when (importProgress)`**

Map new states to status card strings:

| State | UI text |
|-------|---------|
| Selected / Validating | Validating package… |
| ReadingMetadata | Reading metadata… |
| CheckingStorage | Checking storage… |
| Verifying | Verifying install… |
| Registering | Registering game… |
| Installed | same as old Success (refresh list, toast) |
| Failed | show `message` (optionally prefix code for debug builds) |

Keep NeedPasscode / NeedCopyConfirm dialogs.

On LaunchedEffect startup: `InstallCleanup(...).cleanupStaleArtifacts(ImportManager.isBusy())` then `gameRepository.syncLibrary()`.

- [ ] **Step 4: Tests**

```bash
./gradlew :app:testFdroidDebugUnitTest --tests 'com.bachatas4.android.DirectGameLaunchRequestTest'
./gradlew :core:data:testDebugUnitTest
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(install): launch gate and library install states"
```

---

### Task 9: Host regression suite + device checklist

**Files:**
- Ensure all new tests green
- Create: `docs/superpowers/plans/2026-08-06-pkg-install-oneplus-pad2-checklist.md` (manual only)

- [ ] **Step 1: Run all relevant unit tests**

```bash
cd android/BachataS4
./gradlew :core:data:testDebugUnitTest :app:testFdroidDebugUnitTest :feature:library:testDebugUnitTest
```

Expected: PASS (skip modules that do not exist / adjust module names to repo).

- [ ] **Step 2: Write Pad 2 checklist**

```markdown
# OnePlus Pad 2 (OPD2403) install acceptance

Device only: model OPD2403 / OnePlus Pad 2. Do not sign off on other devices.

1. adb devices shows OPD2403 online
2. Install debug APK with packaged runtime
3. Import known-valid PKG → progress states visible → Installed → Launch game
4. Import garbage file → Failed with specific code/message
5. Start PKG import, adb shell am force-stop <pkg> mid-extract → relaunch app → no completed bogus game
6. After successful install, force-stop + reboot → game still listed and launchable
7. Import folder game + PKG game → both launch via same gate
```

- [ ] **Step 3: When device online, execute checklist; record results in checklist file**

If offline: leave unchecked; do not claim device acceptance.

- [ ] **Step 4: Commit tests/docs**

```bash
git commit -m "test(install): host suite and Pad 2 acceptance checklist"
```

---

## Spec coverage checklist

| Spec requirement | Task |
|------------------|------|
| State machine SELECTED…INSTALLED/FAILED | 1, 7, 8 |
| Validate before extract | 7 (+ InstallValidator) |
| Typed errors | 1, 7 |
| Transactional staging + atomic rename | 5 (existing) + 5 manifest |
| install.manifest | 2, 5 |
| job.json resume artifacts | 3, 4, 7 |
| Never register partial | 5, 6 |
| Unified PKG/folder/orphan registration | 5, 6, 7 |
| Launch only INSTALLED | 2, 8 |
| Storage lifecycle / cleanup / reboot | 4, 7, 8 |
| Host tests | 1–6, 8–9 |
| OnePlus Pad 2 only device accept | 9 |

## Self-review notes

- No TBD placeholders in tasks.
- Names consistent: `Installed` not `Success`; `Registering` not `Finalizing`; `Failed(code, message)`.
- `GameInstallVerifier.canLaunch` is the single launch predicate used by library path and `DirectGameLaunchRequest`.
- Resume is best-effort re-extract/re-copy; no byte-level resume (matches spec non-goals).
