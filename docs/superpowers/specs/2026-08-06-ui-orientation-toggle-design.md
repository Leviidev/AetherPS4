# UI Orientation Toggle Design

## Goal

Let the user force portrait or landscape for all non-game Bachata S4 UI (Library, Settings, Setup), independent of the device system rotation lock. Persist the choice across app restarts. Surface a small toggle icon on the Library header next to Settings.

## Non-goals

- No third “follow system sensor” mode.
- No redesign of Settings/Setup layouts beyond natural Compose reflow in landscape.
- No change to in-session touch-controller layout logic.
- Gameplay remains fixed immersive landscape.

## Current state

- `MainActivity` is locked in the manifest to `android:screenOrientation="sensorPortrait"`.
- Entering a session applies `SessionWindowMode.ImmersiveLandscape` (`SCREEN_ORIENTATION_LANDSCAPE`, hide system bars).
- Leaving a session hard-restores `SessionWindowMode.Portrait` (`SCREEN_ORIENTATION_SENSOR_PORTRAIT`) and shows system bars.
- Library header is a title row with logo, “Library”, and a settings gear (`⚙`) on the right.
- SharedPreferences already used for small app prefs (e.g. `emulator_settings` / `RuntimeVulkanDriverPreference`).

## Chosen approach

**App-level preference + Activity `requestedOrientation`** (Approach A).

| Concern | Decision |
|---|---|
| Storage | SharedPreferences |
| Prefs file | `ui_preferences` |
| Key | `ui_orientation` |
| Values | `portrait` (default), `landscape` |
| Apply mechanism | `Activity.requestedOrientation` |
| Scope | All non-session routes |
| Session | Unchanged immersive landscape; dispose restores **saved** preference, not hard-coded portrait |
| Manifest | `sensorPortrait` → `unspecified` so runtime can switch |
| Toggle UI | Library header, left of ⚙ |

## Behavior

### Orientation mapping

| Preference | `ActivityInfo` constant |
|---|---|
| Portrait | `SCREEN_ORIENTATION_SENSOR_PORTRAIT` |
| Landscape | `SCREEN_ORIENTATION_SENSOR_LANDSCAPE` |

Using sensor portrait/landscape allows reverse portrait and reverse landscape while still locking the axis, independent of system auto-rotate being off.

### Toggle control

- Location: Library header row, right end: `[logo] Library … [orientation icon] [⚙]`.
- Tap flips portrait ↔ landscape, writes prefs immediately, applies `requestedOrientation` immediately.
- Icon reflects **current** mode:
  - Portrait preferred → show a portrait-oriented phone glyph (content description: “Switch to landscape”).
  - Landscape preferred → show a landscape-oriented phone glyph (content description: “Switch to portrait”).
- Visual weight matches the existing settings control (small `TextButton` / icon button).
- Prefer Material Icons if the library module already has (or can add) `compose.material.icons`; otherwise a compact unicode glyph consistent with the settings gear is acceptable.

### Session interaction

1. User may be in portrait or landscape UI.
2. Session route: force immersive landscape (existing `SessionWindowModeEffect`).
3. Session dispose / leave route: restore orientation from the persisted preference (not always Portrait); show system bars as today.

### Cold start

- On process start, read preference and apply orientation before or as content is set so the first frame matches the saved choice.
- Default when missing/invalid: portrait.

### Layout adaptation

- Library already uses `GridCells.Adaptive(minSize = 148.dp)` and `BoxWithConstraints`; landscape gains columns automatically.
- Settings/Setup reflow with window size; no dedicated landscape redesign in this change.

## Components

### 1. `UiOrientation` + `UiOrientationPreference`

Small helper (prefer `core/data` or app module if only MainActivity/Library need it; mirror `RuntimeVulkanDriverPreference` style):

```kotlin
enum class UiOrientation {
    Portrait,
    Landscape,
}

object UiOrientationPreference {
    const val FILE_NAME = "ui_preferences"
    const val KEY = "ui_orientation"
    val DEFAULT = UiOrientation.Portrait

    fun decode(value: String?): UiOrientation
    fun encode(value: UiOrientation): String
    fun toActivityOrientation(value: UiOrientation): Int
}
```

- `decode`: null/blank/unknown → `DEFAULT`.
- `encode`: stable lowercase strings `"portrait"` / `"landscape"`.
- Optional thin read/write helpers that take `SharedPreferences` or `Context` are fine if they keep call sites short.

### 2. Apply orientation (non-session)

- Apply from `MainActivity` on create (read prefs → set `requestedOrientation`).
- When the user toggles on Library, write prefs and set `requestedOrientation` on the host Activity immediately.
- A small Compose `DisposableEffect` / helper that resolves `Activity` from `LocalContext` (same pattern as `SessionWindowModeEffect.findActivity`) is fine for the toggle path.

Do **not** re-apply the UI preference while a session is active; session owns orientation for that route.

### 3. Session restore

Update `SessionWindowModeEffect` dispose path:

```text
onDispose {
  activity.requestedOrientation = UiOrientationPreference.toActivityOrientation(saved)
  show system bars
}
```

Read the preference at dispose time (or capture at enter) so a mid-session change is not required; the toggle lives only on Library, so mid-session changes are out of scope.

### 4. Manifest

In `app/src/main/AndroidManifest.xml` for `MainActivity`:

```xml
android:screenOrientation="unspecified"
```

(or remove the attribute). Runtime `requestedOrientation` becomes authoritative for non-session UI.

### 5. Library UI

In the Library header `Row` (alongside the settings `TextButton`):

- Add orientation toggle button before the settings control.
- Wire click → flip, persist, apply.

No change required to game-details sheet Launch / Options / Cancel / Remove actions.

## Data flow

```text
Cold start
  MainActivity.onCreate
    → read ui_preferences.ui_orientation
    → requestedOrientation = mapped constant
    → setContent / NavHost

Library header toggle
  → flip Portrait ↔ Landscape
  → write SharedPreferences
  → requestedOrientation = new mapping

Enter session
  → SessionWindowModeEffect sets LANDSCAPE + hide bars

Leave session
  → dispose: restore preference orientation + show bars
```

## Testing

1. **Unit:** `UiOrientationPreference.decode` / `encode` defaults, valid values, invalid values; `toActivityOrientation` mapping.
2. **Session:** dispose restores preference-backed orientation constant (update any test that assumes hard-coded Portrait restore if present).
3. **Manual / APK:** toggle on Library; rotate lock on device still honors app choice; kill/reopen app restores choice; launch game → landscape; stop session → previous UI orientation.

## Failure handling

- Corrupt or missing pref → portrait default; never crash.
- Activity not found from Compose context → no-op apply (toggle still writes prefs; next cold start applies).

## Out of scope follow-ups

- Per-game UI orientation.
- Settings-screen duplicate toggle.
- Full landscape-optimized Settings chrome.
```
