# UI Orientation Toggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Library-header portrait/landscape toggle that forces non-session UI orientation independent of system auto-rotate, persists the choice, and restores it after gameplay sessions.

**Architecture:** A small `UiOrientationPreference` helper in `core:data` owns encode/decode and SharedPreferences I/O. `MainActivity` applies the preference on cold start; the Library header toggles, persists, and re-applies immediately; `SessionWindowModeEffect` still forces immersive landscape during play and restores the **saved** preference (not hard-coded portrait) on dispose. Manifest unlocks the activity so runtime `requestedOrientation` is authoritative.

**Tech Stack:** Kotlin, Jetpack Compose, AndroidX Activity, SharedPreferences, JUnit 4, Hilt modules already present, Gradle.

## Global Constraints

- Prefs file: `ui_preferences`; key: `ui_orientation`; values: `portrait` | `landscape`; default: portrait.
- Portrait → `ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT`; landscape → `ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE`.
- Preference applies to **all non-session UI**; session remains fixed immersive landscape while composed.
- Independent of system rotation lock: app forces the chosen axis.
- Toggle lives on Library header left of ⚙ only (no Settings duplicate).
- No Settings/Setup landscape redesign; adaptive layouts reflow as-is.
- Before any APK install or publication, follow runtime packaging and `unzip` verification in `AGENTS.md`.

## File Map

| File | Responsibility |
| --- | --- |
| `core/data/.../UiOrientationPreference.kt` | Enum, encode/decode, prefs I/O, Activity orientation mapping. |
| `core/data/.../UiOrientationPreferenceTest.kt` | Unit tests for decode/encode/mapping. |
| `app/src/main/AndroidManifest.xml` | Unlock MainActivity orientation (`unspecified`). |
| `app/.../MainActivity.kt` | Apply saved orientation on cold start. |
| `feature/session/.../SessionWindowModeEffect.kt` | Restore preference orientation on session dispose. |
| `feature/library/.../LibraryScreen.kt` | Header toggle UI + persist + apply. |
| `feature/library/build.gradle.kts` | Optional icons dependency only if Material Icons are used. |

---

### Task 1: `UiOrientationPreference` (TDD)

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/UiOrientationPreference.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/UiOrientationPreferenceTest.kt`

**Interfaces:**
- Consumes: Android `Context`, `SharedPreferences`, `ActivityInfo`
- Produces:
  - `enum class UiOrientation { Portrait, Landscape }`
  - `object UiOrientationPreference` with `FILE_NAME`, `KEY`, `DEFAULT`, `decode`, `encode`, `toActivityOrientation`, `read`, `write`, `toggle`

- [ ] **Step 1: Write the failing unit tests**

Create `UiOrientationPreferenceTest.kt`:

```kotlin
package com.bachatas4.android.data

import android.content.pm.ActivityInfo
import org.junit.Assert.assertEquals
import org.junit.Test

class UiOrientationPreferenceTest {
    @Test
    fun decodeDefaultsWhenMissingOrInvalid() {
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode(null))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode(""))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("   "))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("unknown"))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("PORTRAIT"))
    }

    @Test
    fun decodeAcceptsStoredValues() {
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("portrait"))
        assertEquals(UiOrientation.Landscape, UiOrientationPreference.decode("landscape"))
    }

    @Test
    fun encodeIsStableLowercase() {
        assertEquals("portrait", UiOrientationPreference.encode(UiOrientation.Portrait))
        assertEquals("landscape", UiOrientationPreference.encode(UiOrientation.Landscape))
    }

    @Test
    fun mapsToSensorAxisLocks() {
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT,
            UiOrientationPreference.toActivityOrientation(UiOrientation.Portrait),
        )
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
            UiOrientationPreference.toActivityOrientation(UiOrientation.Landscape),
        )
    }

    @Test
    fun toggleFlipsBothWays() {
        assertEquals(UiOrientation.Landscape, UiOrientationPreference.toggle(UiOrientation.Portrait))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.toggle(UiOrientation.Landscape))
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest --tests com.bachatas4.android.data.UiOrientationPreferenceTest
```

Expected: FAIL — `UiOrientationPreference` / `UiOrientation` unresolved.

- [ ] **Step 3: Implement the preference helper**

Create `UiOrientationPreference.kt`:

```kotlin
package com.bachatas4.android.data

import android.content.Context
import android.content.pm.ActivityInfo

enum class UiOrientation {
    Portrait,
    Landscape,
}

object UiOrientationPreference {
    const val FILE_NAME = "ui_preferences"
    const val KEY = "ui_orientation"
    val DEFAULT = UiOrientation.Portrait

    fun decode(value: String?): UiOrientation = when (value?.trim()) {
        "portrait" -> UiOrientation.Portrait
        "landscape" -> UiOrientation.Landscape
        else -> DEFAULT
    }

    fun encode(value: UiOrientation): String = when (value) {
        UiOrientation.Portrait -> "portrait"
        UiOrientation.Landscape -> "landscape"
    }

    fun toActivityOrientation(value: UiOrientation): Int = when (value) {
        UiOrientation.Portrait -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        UiOrientation.Landscape -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    }

    fun toggle(value: UiOrientation): UiOrientation = when (value) {
        UiOrientation.Portrait -> UiOrientation.Landscape
        UiOrientation.Landscape -> UiOrientation.Portrait
    }

    fun read(context: Context): UiOrientation {
        val prefs = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
        return decode(prefs.getString(KEY, null))
    }

    fun write(context: Context, value: UiOrientation) {
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY, encode(value))
            .apply()
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest --tests com.bachatas4.android.data.UiOrientationPreferenceTest
```

Expected: PASS (all five tests).

- [ ] **Step 5: Commit**

```bash
git add \
  android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/UiOrientationPreference.kt \
  android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/UiOrientationPreferenceTest.kt
git commit -m "$(cat <<'EOF'
feat(android): add UiOrientationPreference helper

Persist portrait/landscape UI choice with stable encode/decode
and Activity orientation mapping.
EOF
)"
```

---

### Task 2: Cold-start apply + unlock manifest

**Files:**
- Modify: `android/BachataS4/app/src/main/AndroidManifest.xml`
- Modify: `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/MainActivity.kt`

**Interfaces:**
- Consumes: `UiOrientationPreference.read`, `UiOrientationPreference.toActivityOrientation`
- Produces: MainActivity applies saved orientation before `setContent`

- [ ] **Step 1: Unlock activity orientation in the manifest**

In `android/BachataS4/app/src/main/AndroidManifest.xml`, change MainActivity from:

```xml
android:screenOrientation="sensorPortrait">
```

to:

```xml
android:screenOrientation="unspecified">
```

- [ ] **Step 2: Apply preference on cold start in MainActivity**

In `MainActivity.onCreate`, after `super.onCreate(savedInstanceState)` and before `setContent`, apply:

```kotlin
import com.bachatas4.android.data.UiOrientationPreference

// inside onCreate, after super.onCreate(...):
val uiOrientation = UiOrientationPreference.read(this)
requestedOrientation = UiOrientationPreference.toActivityOrientation(uiOrientation)
```

Full `onCreate` should look like:

```kotlin
override fun onCreate(savedInstanceState: Bundle?) {
    enableEdgeToEdge()
    super.onCreate(savedInstanceState)
    val uiOrientation = UiOrientationPreference.read(this)
    requestedOrientation = UiOrientationPreference.toActivityOrientation(uiOrientation)
    lifecycleScope.launch { legacyRuntimeSettingsMigration.migrate() }
    val runtimeRoot = java.io.File(filesDir, "runtime")
    val isRuntimeInstalled = runtimeRoot.listFiles()?.any { it.isDirectory && it.name.startsWith("box64-") } == true
    setContent {
        AppTheme {
            Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                BachataNavHost(startDestination = initialRouteForSoc(Build.SOC_MODEL, isRuntimeInstalled))
            }
        }
    }
}
```

- [ ] **Step 3: Compile app module**

Run:

```bash
cd android/BachataS4 && ./gradlew :app:compilePlaystoreDebugKotlin
```

Expected: BUILD SUCCESSFUL (or `compileFdroidDebugKotlin` if playstore flavor is unavailable — use the flavor that matches local config; either is fine if both compile).

If both flavors exist, prefer:

```bash
cd android/BachataS4 && ./gradlew :app:compilePlaystoreDebugKotlin :app:compileFdroidDebugKotlin
```

- [ ] **Step 4: Commit**

```bash
git add \
  android/BachataS4/app/src/main/AndroidManifest.xml \
  android/BachataS4/app/src/main/kotlin/com/bachatas4/android/MainActivity.kt
git commit -m "$(cat <<'EOF'
feat(android): apply UI orientation on cold start

Unlock MainActivity orientation and honor the persisted
portrait/landscape preference before Compose content.
EOF
)"
```

---

### Task 3: Session dispose restores saved preference

**Files:**
- Modify: `android/BachataS4/feature/session/src/main/kotlin/com/bachatas4/android/feature/session/SessionWindowModeEffect.kt`
- Modify (only if assertions need updating): `android/BachataS4/feature/session/src/test/kotlin/com/bachatas4/android/feature/session/SessionWindowModeTest.kt`

**Interfaces:**
- Consumes: `UiOrientationPreference.read(context)`, `UiOrientationPreference.toActivityOrientation`
- Produces: On session dispose, `requestedOrientation` matches saved preference; system bars still restored

- [ ] **Step 1: Update SessionWindowModeEffect dispose path**

Replace hard-coded portrait restore with preference restore. Keep immersive landscape on enter and system-bar show on dispose.

```kotlin
package com.bachatas4.android.feature.session

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.bachatas4.android.data.UiOrientationPreference

@Composable
fun SessionWindowModeEffect() {
    val context = LocalContext.current
    val view = LocalView.current
    DisposableEffect(context, view) {
        val activity = context.findActivity()
        val controller = activity?.window?.let { WindowCompat.getInsetsController(it, view) }
        activity?.requestedOrientation = SessionWindowMode.ImmersiveLandscape.orientation
        controller?.let {
            it.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            if (SessionWindowMode.ImmersiveLandscape.hideSystemBars) {
                it.hide(WindowInsetsCompat.Type.systemBars())
            }
        }
        onDispose {
            val restored = UiOrientationPreference.read(context)
            activity?.requestedOrientation = UiOrientationPreference.toActivityOrientation(restored)
            controller?.show(WindowInsetsCompat.Type.systemBars())
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.takeUnless { it === this }?.findActivity()
    else -> null
}
```

- [ ] **Step 2: Keep SessionWindowMode enum tests as policy documentation**

`SessionWindowModeTest` still documents `SessionWindowMode.Portrait` / `ImmersiveLandscape` constants. **Do not** change those constants unless something else breaks. Add a short KDoc on `SessionWindowMode.Portrait` noting it is the default UI policy and cold-start default; runtime restore after session now uses `UiOrientationPreference`.

Optional comment on the enum:

```kotlin
enum class SessionWindowMode(
    val orientation: Int,
    val hideSystemBars: Boolean,
) {
    /** Default non-session axis lock (also UiOrientationPreference default). */
    Portrait(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT, false),
    ImmersiveLandscape(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, true),
}
```

- [ ] **Step 3: Run session unit tests**

Run:

```bash
cd android/BachataS4 && ./gradlew :feature:session:testDebugUnitTest
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add \
  android/BachataS4/feature/session/src/main/kotlin/com/bachatas4/android/feature/session/SessionWindowModeEffect.kt \
  android/BachataS4/feature/session/src/main/kotlin/com/bachatas4/android/feature/session/SessionWindowMode.kt
git commit -m "$(cat <<'EOF'
fix(android): restore UI orientation after session

Session dispose re-applies the persisted portrait/landscape
preference instead of always forcing portrait.
EOF
)"
```

---

### Task 4: Library header orientation toggle

**Files:**
- Modify: `android/BachataS4/feature/library/src/main/kotlin/com/bachatas4/android/feature/library/LibraryScreen.kt`
- Modify: `android/BachataS4/feature/library/build.gradle.kts` only if adding Material Icons (prefer matching ⚙ with unicode glyphs to avoid a new dependency)

**Interfaces:**
- Consumes: `UiOrientationPreference.read/write/toggle/toActivityOrientation`, Activity via `LocalContext`
- Produces: Header control that flips, persists, and applies orientation

- [ ] **Step 1: Add a small Activity finder helper in LibraryScreen (private)**

Near other private helpers in `LibraryScreen.kt`, add:

```kotlin
import android.app.Activity
import android.content.ContextWrapper
import com.bachatas4.android.data.UiOrientation
import com.bachatas4.android.data.UiOrientationPreference

private tailrec fun android.content.Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.takeUnless { it === this }?.findActivity()
    else -> null
}
```

- [ ] **Step 2: Wire toggle state and header button**

Inside `LibraryContent` (or the header `item` block where context is available), track the preference:

```kotlin
var uiOrientation by remember {
    mutableStateOf(UiOrientationPreference.read(context))
}
```

In the header `Row`, **before** the settings `TextButton`, insert:

```kotlin
TextButton(
    onClick = {
        val next = UiOrientationPreference.toggle(uiOrientation)
        UiOrientationPreference.write(context, next)
        context.findActivity()?.requestedOrientation =
            UiOrientationPreference.toActivityOrientation(next)
        uiOrientation = next
    },
) {
    // Current mode glyph; tap switches to the other axis.
    // Portrait: tall rectangle; landscape: wide rectangle (matches ⚙ emoji style).
    Text(
        text = if (uiOrientation == UiOrientation.Portrait) "▯" else "▭",
        color = BachataPalette.Primary,
        style = MaterialTheme.typography.titleLarge,
    )
}
```

Use content description via semantics for accessibility:

```kotlin
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics

Text(
    text = if (uiOrientation == UiOrientation.Portrait) "▯" else "▭",
    color = BachataPalette.Primary,
    style = MaterialTheme.typography.titleLarge,
    modifier = Modifier.semantics {
        contentDescription = if (uiOrientation == UiOrientation.Portrait) {
            "Switch to landscape"
        } else {
            "Switch to portrait"
        }
    },
)
```

Final header actions order:

```text
[logo] Library ........ [orientation] [⚙]
```

- [ ] **Step 3: Compile library module**

Run:

```bash
cd android/BachataS4 && ./gradlew :feature:library:compileDebugKotlin
```

Expected: BUILD SUCCESSFUL.

- [ ] **Step 4: Commit**

```bash
git add android/BachataS4/feature/library/src/main/kotlin/com/bachatas4/android/feature/library/LibraryScreen.kt
git commit -m "$(cat <<'EOF'
feat(android): add library orientation toggle

Header control flips and persists portrait/landscape and
applies requestedOrientation immediately.
EOF
)"
```

---

### Task 5: Verification

**Files:** none new — run existing test suites for touched modules.

- [ ] **Step 1: Run unit tests for touched modules**

```bash
cd android/BachataS4 && ./gradlew \
  :core:data:testDebugUnitTest \
  :feature:session:testDebugUnitTest \
  :feature:library:testDebugUnitTest \
  :app:testPlaystoreDebugUnitTest
```

If playstore unit tests are unavailable, run `:app:testFdroidDebugUnitTest` instead (or both).

Expected: BUILD SUCCESSFUL; all tests PASS.

- [ ] **Step 2: Lint touched surface (optional but preferred)**

```bash
cd android/BachataS4 && ./gradlew :app:lintPlaystoreDebug
```

Expected: no new orientation-related lint errors. If lint is slow, at minimum ensure compile succeeds for app + library + session + data.

- [ ] **Step 3: Manual check list (device or emulator)**

1. Fresh install / clear app data → Library opens portrait.
2. Tap orientation control → UI rotates to landscape; grid gains columns.
3. Force-stop app and reopen → still landscape.
4. Tap again → portrait; reopen → portrait.
5. With system auto-rotate **off**, toggle still switches app orientation.
6. Launch a game → immersive landscape session.
7. Stop / leave session → returns to the orientation chosen in step 2/4 (not always portrait).
8. Open Settings from Library while landscape → Settings stays landscape.

- [ ] **Step 4: Final commit only if verification required small fixes**

If Step 1–2 required fixes, commit those fixes with a clear message. If everything already passed, no extra commit.

---

## Self-review (plan vs spec)

| Spec requirement | Task |
| --- | --- |
| SharedPreferences `ui_preferences` / `ui_orientation` | Task 1 |
| portrait/landscape encode + default | Task 1 |
| SENSOR_PORTRAIT / SENSOR_LANDSCAPE mapping | Task 1 |
| Manifest unlock | Task 2 |
| Cold-start apply | Task 2 |
| Session immersive landscape unchanged | Task 3 (enter path untouched) |
| Session dispose restores preference | Task 3 |
| Library header toggle left of ⚙ | Task 4 |
| Persist + immediate apply on toggle | Task 4 |
| Independent of system rotation | Task 2+4 `requestedOrientation` |
| Unit tests for preference | Task 1 |
| No Settings redesign / no sensor-follow mode | Out of scope; no tasks |

No placeholders remain; method names are consistent across tasks (`read`/`write`/`toggle`/`toActivityOrientation`/`decode`/`encode`).
