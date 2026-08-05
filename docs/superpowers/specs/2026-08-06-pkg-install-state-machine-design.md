# PKG Install State Machine Design

## Goal

Make PKG (and folder) installation a first-class, transactional onboarding path so a known-valid package installs and launches without manual pre-installed game folders. Installation is separated from launch: launch is enabled only after a verified `INSTALLED` registration.

This design evolves the existing import stack (Approach A). It does not introduce a parallel installer module.

## Background

Existing path (see `docs/superpowers/specs/2026-07-23-pkg-import-design.md`):

1. Library picks folder (SAF tree) or PKG (document).
2. `ImportService` copies or native-extracts into `files/games/.import-<uuid>`.
3. `ContentImporter` atomic-renames into `files/games/<id>`.
4. `GameRepository.addImportedGame` inserts a Room row.
5. `ImportProgress` reports Preparing / Scanning / Extracting / Copying / Finalizing / Success / Failed (plus passcode and storage confirm gates).

Gaps versus product requirements:

| Requirement | Current gap |
|-------------|-------------|
| Explicit install state machine | Progress names differ; no persisted job state |
| Validate before extract | Probe + free-space only; weak typed errors |
| Transactional install + manifest | Staging + rename exist; no install manifest; weak verify |
| Never register partial games | DB insert is post-rename, but `syncOrphanedFolders()` can register incomplete trees |
| Resumable / reboot-safe | Progress is in-memory only; staging cleaned when idle |
| Launch only when installed | Any DB row is launchable |
| Unified registration | Folder/PKG finalize diverge from orphan scan |

## Decisions (locked)

| Topic | Choice |
|-------|--------|
| Architecture | **Approach A:** evolve `ImportProgress` into the formal install state machine; keep `ImportService` / `ImportManager` / library import card |
| Install root | App-private `context.filesDir/games/` only |
| Registration | Single path: verify → atomic promote → write/confirm manifest → DB insert |
| Launch gate | DB row + on-disk `install.manifest` with `status=INSTALLED` + `eboot.bin` |
| Resume | Safe resume only (re-extract/re-copy when cache/source still valid); no mid-file byte resume |
| Device test matrix | **OnePlus Pad 2 (`OPD2403`) only** for device acceptance |
| Out of scope | Multi-PKG queue; patch delta apply beyond base-must-exist; new installer module |

## State machine

`ImportProgress` is the install state surface (UI + service + notification).

| Spec state | `ImportProgress` | Meaning |
|------------|------------------|---------|
| SELECTED | `Selected(uri, mode)` | Slot claimed; source URI recorded |
| VALIDATING | `Validating(...)` | Accessibility, type, header |
| READING_METADATA | `ReadingMetadata(...)` | Probe / SFO / title & content id |
| CHECKING_STORAGE | `CheckingStorage(...)` | Free space, dest writable, partial/duplicate, base/patch |
| (user gate) | `NeedCopyConfirm(...)` | Optional confirm when peak storage large |
| (user gate) | `NeedPasscode(...)` | PKG auth required |
| EXTRACTING | `Extracting(...)` / `Copying(...)` | PKG extract or folder tree copy into staging |
| VERIFYING | `Verifying(title)` | Required files + install manifest in staging |
| REGISTERING | `Registering(title)` | Atomic rename + DB insert (replaces `Finalizing`) |
| INSTALLED | `Installed(gameId, title)` | Terminal success (replaces `Success`) |
| FAILED | `Failed(code, message)` | Terminal failure with typed code |

**Busy rules:** all non-terminal states except `Idle` block a second import (`tryBeginImport` false). Terminal: `Installed`, `Failed`, `Idle`.

**Launch enable:** only after `Installed` and library shows a game that passes `canLaunch` (below). In-progress installs never appear as completed library games.

Compatibility aliases for tests/UI during migration:

- `Finalizing` → `Registering`
- `Success` → `Installed`

New code must use the new names; update call sites in the same change set.

## Pipeline

```
Library UI
  ├─ Folder → OpenDocumentTree → ImportService mode=folder
  └─ PKG    → OpenDocument     → ImportService mode=pkg
         │
         ▼
  tryBeginImport → Selected
  write job.json (InstallJobStore)
         │
         ▼
  Validating → open URI / type / header
  ReadingMetadata → probe or source SFO
  CheckingStorage → space, writable, partial, base
         │
    NeedCopyConfirm? ──► user confirm
         │
         ▼
  staging = games/.import-<jobId>
  Extracting (PKG) or Copying (folder)
    NeedPasscode? ──► user passcode ──► retry extract
         │
         ▼
  Verifying → required files + install.manifest.tmp
  Registering → ATOMIC_MOVE → GameRegistration.insert
  Installed
```

Cancel: `ACTION_CANCEL` → native cancel if extracting → cleanup job/staging/cache → `Failed(CANCELLED)` or return to `Idle` after cleanup (prefer `Failed(CANCELLED)` once so UI can show message, then user dismiss resets to `Idle`).

## Validation (before extraction / copy)

Hard fail with typed `InstallErrorCode` before staging payload write (and before PKG local cache copy when used):

| Order | Check | Error |
|------:|-------|--------|
| 1 | Source URI openable | `SOURCE_INACCESSIBLE` / `PERMISSION_LOST` |
| 2 | Supported package type (`.pkg` document or folder tree) | `UNSUPPORTED_TYPE` |
| 3 | PKG header via `nativeProbe` | `BAD_HEADER` / `MALFORMED_PACKAGE` |
| 4 | Unsupported encryption / unreadable crypto path | `UNSUPPORTED_ENCRYPTION` |
| 5 | Game/title identifier (content id / title id) | `NO_TITLE_ID` |
| 6 | Destination `games/` writable under app filesDir | `DEST_NOT_WRITABLE` |
| 7 | Required free space (package+extract peak + margin for PKG path that caches; extract-only peak when stream path applies) | `INSUFFICIENT_STORAGE` |
| 8 | Existing destination: complete installed → `ALREADY_INSTALLED`; incomplete partial → purge if owned/incomplete, else `PARTIAL_EXISTS` | see duplicate rules |
| 9 | Patch/DLC requires installed verified base | `BASE_MISSING` |

Folder mode additionally requires source tree to contain `eboot.bin` and `sce_sys/param.sfo` before copy starts.

## Error codes

```text
SOURCE_INACCESSIBLE
UNSUPPORTED_TYPE
BAD_HEADER
NO_TITLE_ID
INSUFFICIENT_STORAGE
DEST_NOT_WRITABLE
PARTIAL_EXISTS
BASE_MISSING
UNSUPPORTED_ENCRYPTION
MALFORMED_PACKAGE
VERIFY_FAILED
ALREADY_INSTALLED
INTERRUPTED
PERMISSION_LOST
CANCELLED
UNKNOWN
```

`Failed(code, message)` carries human-readable `message` for UI; logs include code + message. Map existing free-text failures onto codes during the change.

## Transactional install

### Layout

```text
files/games/
  .jobs/<jobId>/job.json
  .import-<jobId>/                 # staging only
    eboot.bin
    sce_sys/param.sfo
    install.manifest.tmp
    ...
  <titleId>/                       # final only after ATOMIC_MOVE
    eboot.bin
    sce_sys/param.sfo
    install.manifest               # status=INSTALLED
    ...
files/pkg-cache/<jobId>.pkg        # optional local sequential cache for PKG
```

### Rules

1. Never write final payload outside staging until verify passes.
2. VERIFY writes `install.manifest.tmp` inside staging (files checklist, sizes, ids, mode, sourceUri).
3. ATOMIC_MOVE staging → `games/<id>` (existing `ContentImporter.moveAtomically`; rename manifest tmp → `install.manifest` before or as part of promote — prefer write final name `install.manifest` inside staging before move so rename is single directory move).
4. DB insert only after successful promote and on-disk manifest `status=INSTALLED`.
5. Never reverse order (no DB row before files).
6. Failure/cancel: delete staging, optional cache, job record; never leave a Success/Installed-looking game.

### job.json

Path: `files/games/.jobs/<jobId>/job.json`

```json
{
  "version": 1,
  "jobId": "uuid",
  "state": "CHECKING_STORAGE",
  "mode": "pkg",
  "sourceUri": "content://...",
  "uriPersistable": true,
  "displayName": "...",
  "contentId": "EP....",
  "titleId": "CUSA....",
  "stagingDir": "games/.import-<jobId>",
  "cachePath": "pkg-cache/<jobId>.pkg",
  "packageBytes": 0,
  "extractBytes": 0,
  "requiredBytes": 0,
  "createdAtMs": 0,
  "updatedAtMs": 0,
  "lastErrorCode": null,
  "lastErrorMessage": null
}
```

Atomic write: `job.json.tmp` then rename. Update `state` and `updatedAtMs` on every transition.

### install.manifest

Path: `games/<id>/install.manifest`

```json
{
  "version": 1,
  "status": "INSTALLED",
  "gameId": "...",
  "contentId": "...",
  "mode": "pkg",
  "sourceUri": "...",
  "installedAtMs": 0,
  "requiredFiles": ["eboot.bin", "sce_sys/param.sfo"],
  "bytesTotal": 0
}
```

### VERIFY checklist (shared)

- `eboot.bin` exists and non-empty
- `sce_sys/param.sfo` parseable; resolved title id matches destination id policy
- All paths under staging (no escape)
- Manifest fields complete
- Total size > 0

Failure → cleanup → `Failed(VERIFY_FAILED)`.

## Resume policy

On process start / service restart:

| Prior state | Action |
|-------------|--------|
| SELECTED … CHECKING_STORAGE | Restart validation from source URI if still openable; else `PERMISSION_LOST` + cleanup |
| EXTRACTING with complete pkg cache | Delete partial staging; re-extract from cache |
| EXTRACTING without cache | Restart from source if openable; else fail |
| COPYING | Delete staging; re-copy from source if openable |
| VERIFYING / REGISTERING | If staging complete → verify + promote + register; if dest exists without DB → verify dest + register or purge incomplete |
| Terminal / corrupt job | Cleanup job artifacts; ignore |

Passcode waiters are not persisted; if keys missing after resume, show `NeedPasscode` again.

At most one active job. Stale jobs cleaned when `!ImportManager.isBusy()`.

## Unified registration path

New helper (name illustrative): `GameRegistration` / methods on `GameRepository` + `ContentImporter`:

```text
registerInstalled(dir, metadata, sourceUri, mode) 
  → verify tree + manifest
  → insert GameEntity
```

| Entry | Path |
|-------|------|
| PKG import | extract → verify → atomic move → registerInstalled |
| Folder import | copy → verify → atomic move → registerInstalled |
| Pre-installed / orphan folder | **must** pass verify; heal missing manifest when tree complete; **never** insert incomplete trees |

### Replace `syncOrphanedFolders`

Current behavior inserts any non-dot folder, optionally without valid eboot — **forbidden**.

New `syncLibrary()`:

1. Skip `.import-*`, `.jobs`, other dot dirs
2. If tree verifies as complete:
   - Ensure `install.manifest` (heal write if missing)
   - Insert DB row if absent
3. If incomplete:
   - Do not insert
   - If matches active/failed job staging ownership patterns, cleanup via `InstallCleanup`
   - Otherwise leave on disk and log (no silent wipe of unknown user content)
4. Drop DB rows whose files fail `canLaunch` verification when files missing/corrupt (prefer remove row; do not delete user files unless incomplete job-owned)

## Launch gate

```text
canLaunch(game):
  DB row exists
  AND filesDir/relativePath is directory
  AND install.manifest exists with status=INSTALLED
  AND eboot.bin exists and non-empty
```

- Library / direct launch must refuse when false and surface a specific reason.
- Import progress UI stays on the import card until `Installed`; incomplete installs are not library tiles.

## Storage lifecycle

| Scenario | Behavior |
|----------|----------|
| SAF URI | Prefer `takePersistableUriPermission`; record `uriPersistable` on job |
| Persist fails | Session grant only; resume may require re-pick → `PERMISSION_LOST` |
| Scoped storage | All games under app `filesDir/games/` |
| External source removed | `SOURCE_INACCESSIBLE` / `PERMISSION_LOST` + cleanup job artifacts |
| Force-stop / reboot mid-install | Reconcile jobs; resume if safe else cleanup + not registered |
| Low storage mid-work | Fail typed; cleanup staging/cache |
| Duplicate install | Complete+DB → `ALREADY_INSTALLED`; complete no DB → heal register; incomplete → purge incomplete then allow |
| App update | Room + files preserved; `version: 1` manifests/jobs; unknown version → do not treat as installed |

### Cleanup

`InstallCleanup`:

- `cleanupJob(jobId)` — staging, cache, job.json
- `reconcile()` on startup — stale jobs, orphan `.import-*` when idle, apply resume policy once
- User `deleteGame` — removes final tree + DB row (existing path); also remove any matching job leftovers

## Components (file-level)

| Piece | Role |
|-------|------|
| `ImportProgress` / `ImportManager` | State machine + busy slot |
| `InstallErrorCode` | Typed failures |
| `InstallJobStore` | Persist/load/update/delete `job.json` |
| `InstallManifest` | Read/write/verify manifest |
| `InstallCleanup` | Failed/stale artifact cleanup + startup reconcile |
| `ContentImporter` | Staging, verify, atomic move; shared by modes |
| `GameRepository` / registration helper | Sole DB insert path after verify |
| `ImportService` | Drive transitions; PKG + folder |
| Library / launch entry | `canLaunch` gate |
| Unit tests | State, validate mapping, transactional register, orphan rules, canLaunch |

## Success criteria (acceptance)

1. Known-valid PKG installs and launches without manual folder intervention.
2. Invalid packages fail with a specific `InstallErrorCode` (and user-visible message).
3. Interrupted installations cannot appear as completed games.
4. Installed games remain registered after force-stop and reboot.
5. PKG-import and pre-installed-folder workflows launch through the same validated registration path.
6. Installer crashes eliminated on the reference device: **OnePlus Pad 2 only** for device verification.

## Host tests (required)

- State transition + busy rules for new states
- Validation order → error codes
- Manifest + atomic register; no DB on verify fail
- Crash between rename and insert: heal on reconcile
- Incomplete orphan folder not registered
- Complete pre-installed folder heals manifest + registers
- `canLaunch` false without manifest or eboot

## Device tests (OnePlus Pad 2 / OPD2403 only)

When device is online:

1. Valid PKG → install → launch
2. Invalid file → specific error
3. Kill mid-extract → no completed game; resume or clean fail
4. Force-stop after installed → still registered and launchable
5. Folder import and PKG both pass same launch gate

Do not claim device acceptance on any other model.

## Relationship to prior PKG import design

This document extends `2026-07-23-pkg-import-design.md`. Crypto order, native extract host, and KeyDB behavior remain as specified there unless this document overrides. Overrides: progress model names, mandatory install manifest, registration sole path, orphan sync rules, launch gate, persisted job state.

## Non-goals

- Multi-job install queue
- Byte-level extract resume inside a single file
- Desktop-side pre-extract requirement
- Changing runtime launch/GPU stack beyond install/launch gating
- Testing on devices other than OnePlus Pad 2 for this work’s acceptance
