package com.bachatas4.android.feature.settings.input

import com.bachatas4.android.runtime.input.AxisDirection
import com.bachatas4.android.runtime.input.PhysicalBinding
import com.bachatas4.android.runtime.input.PhysicalBindingKind

/**
 * Human-readable names for physical controller bindings, so the UI doesn't show raw
 * "BUTTON 96" / "AXIS 0" strings. Uses Xbox-style naming since most Android controllers
 * identify as Xbox gamepads.
 */
private val BUTTON_NAMES = mapOf(
    // D-Pad keycodes
    19 to "Dpad ↑", 20 to "Dpad ↓", 21 to "Dpad ←", 22 to "Dpad →",
    // Face buttons (Xbox layout: KEYCODE_BUTTON_A/B/X/Y)
    96 to "A", 97 to "B", 98 to "Back", 99 to "X", 100 to "Y",
    101 to "Guide", 108 to "Menu", 109 to "View", 188 to "Home",
    // Shoulder / trigger buttons
    102 to "LB", 103 to "RB", 104 to "LT", 105 to "RT",
    // Stick clicks
    106 to "LS", 107 to "RS",
)

private val AXIS_NAMES = mapOf(
    0 to "L-Stick X", 1 to "L-Stick Y",
    11 to "R-Stick X", 14 to "R-Stick Y",
    15 to "HAT X", 16 to "HAT Y",
    17 to "LT", 18 to "RT",
)

/** Friendly name for a [PhysicalBinding], e.g. "A", "HAT Y −", "L-Stick X". */
fun PhysicalBinding.displayText(): String = when {
    kind == PhysicalBindingKind.BUTTON -> BUTTON_NAMES[code] ?: "Btn $code"
    kind == PhysicalBindingKind.AXIS -> {
        val base = AXIS_NAMES[code] ?: "Axis $code"
        when (direction) {
            AxisDirection.NEGATIVE -> "$base −"
            AxisDirection.POSITIVE -> "$base +"
            else -> base
        }
    }
    else -> "Btn $code"
}

/** Short label without direction suffix, for compact display. */
fun PhysicalBinding.displayShort(): String = when {
    kind == PhysicalBindingKind.BUTTON -> BUTTON_NAMES[code] ?: "Btn $code"
    kind == PhysicalBindingKind.AXIS -> AXIS_NAMES[code] ?: "Axis $code"
    else -> "Btn $code"
}

/** True if this binding represents a D-Pad HAT axis. */
fun PhysicalBinding.isHatAxis(): Boolean =
    kind == PhysicalBindingKind.AXIS && (code == 15 || code == 16)
