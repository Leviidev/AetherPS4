package com.bachatas4.android.runtime.input

import kotlinx.serialization.Serializable

@Serializable
data class ControllerDeviceKey(
    val descriptor: String,
    val vendorId: Int,
    val productId: Int,
    val fallbackName: String,
) {
    init { require(descriptor.isNotBlank() || fallbackName.isNotBlank()) { "Controller identity is blank" } }
}

@Serializable
enum class PhysicalBindingKind { BUTTON, AXIS }

/** Which sign of an axis value activates the binding. BOTH = either direction. */
@Serializable
enum class AxisDirection { BOTH, POSITIVE, NEGATIVE }

@Serializable
data class PhysicalBinding(
    val kind: PhysicalBindingKind,
    val code: Int,
    val direction: AxisDirection = AxisDirection.BOTH,
) {
    init { require(code >= 0) { "Controller binding code is invalid" } }
}

@Serializable
data class ControllerProfile(
    val device: ControllerDeviceKey? = null,
    val bindings: Map<String, PhysicalBinding> = emptyMap(),
    val deadZone: Float = 0.08f,
    val invertAxes: Set<String> = emptySet(),
    val triggerThreshold: Float = 0.5f,
    val vibrationEnabled: Boolean = true,
    val motionEnabled: Boolean = false,
) {
    init {
        require(deadZone.isFinite() && deadZone in 0f..1f) { "Dead zone must be 0..1" }
        require(triggerThreshold.isFinite() && triggerThreshold in 0f..1f) { "Trigger threshold must be 0..1" }
        require(bindings.keys.all { it in LOGICAL_CONTROLS }) { "Unknown logical controller binding" }
    }

    companion object {
        val LOGICAL_CONTROLS = setOf(
            "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger",
            "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
            "dpad_up", "dpad_down", "dpad_left", "dpad_right", "options", "share", "touchpad", "ps",
        )

        fun standard(device: ControllerDeviceKey? = null): ControllerProfile = ControllerProfile(
            device = device,
            bindings = mapOf(
                "left_x" to PhysicalBinding(PhysicalBindingKind.AXIS, 0), "left_y" to PhysicalBinding(PhysicalBindingKind.AXIS, 1),
                "right_x" to PhysicalBinding(PhysicalBindingKind.AXIS, 11), "right_y" to PhysicalBinding(PhysicalBindingKind.AXIS, 14),
                "left_trigger" to PhysicalBinding(PhysicalBindingKind.AXIS, 17), "right_trigger" to PhysicalBinding(PhysicalBindingKind.AXIS, 18),
                "cross" to PhysicalBinding(PhysicalBindingKind.BUTTON, 96), "circle" to PhysicalBinding(PhysicalBindingKind.BUTTON, 97),
                "square" to PhysicalBinding(PhysicalBindingKind.BUTTON, 99), "triangle" to PhysicalBinding(PhysicalBindingKind.BUTTON, 100),
                "l1" to PhysicalBinding(PhysicalBindingKind.BUTTON, 102), "r1" to PhysicalBinding(PhysicalBindingKind.BUTTON, 103),
                "l2" to PhysicalBinding(PhysicalBindingKind.BUTTON, 104), "r2" to PhysicalBinding(PhysicalBindingKind.BUTTON, 105),
                "l3" to PhysicalBinding(PhysicalBindingKind.BUTTON, 106), "r3" to PhysicalBinding(PhysicalBindingKind.BUTTON, 107),
                "options" to PhysicalBinding(PhysicalBindingKind.BUTTON, 108), "share" to PhysicalBinding(PhysicalBindingKind.BUTTON, 109),
                "dpad_up" to PhysicalBinding(PhysicalBindingKind.BUTTON, 19), "dpad_down" to PhysicalBinding(PhysicalBindingKind.BUTTON, 20),
                "dpad_left" to PhysicalBinding(PhysicalBindingKind.BUTTON, 21), "dpad_right" to PhysicalBinding(PhysicalBindingKind.BUTTON, 22),
                "ps" to PhysicalBinding(PhysicalBindingKind.BUTTON, 188),
            ),
        )

        /** Default mapping for controllers that expose the D-Pad as HAT axes (Xbox-style). */
        fun standardWithHatDpad(device: ControllerDeviceKey? = null): ControllerProfile = ControllerProfile(
            device = device,
            bindings = mapOf(
                "left_x" to PhysicalBinding(PhysicalBindingKind.AXIS, 0), "left_y" to PhysicalBinding(PhysicalBindingKind.AXIS, 1),
                "right_x" to PhysicalBinding(PhysicalBindingKind.AXIS, 11), "right_y" to PhysicalBinding(PhysicalBindingKind.AXIS, 14),
                "left_trigger" to PhysicalBinding(PhysicalBindingKind.AXIS, 17), "right_trigger" to PhysicalBinding(PhysicalBindingKind.AXIS, 18),
                "cross" to PhysicalBinding(PhysicalBindingKind.BUTTON, 96), "circle" to PhysicalBinding(PhysicalBindingKind.BUTTON, 97),
                "square" to PhysicalBinding(PhysicalBindingKind.BUTTON, 99), "triangle" to PhysicalBinding(PhysicalBindingKind.BUTTON, 100),
                "l1" to PhysicalBinding(PhysicalBindingKind.BUTTON, 102), "r1" to PhysicalBinding(PhysicalBindingKind.BUTTON, 103),
                "l2" to PhysicalBinding(PhysicalBindingKind.BUTTON, 104), "r2" to PhysicalBinding(PhysicalBindingKind.BUTTON, 105),
                "l3" to PhysicalBinding(PhysicalBindingKind.BUTTON, 106), "r3" to PhysicalBinding(PhysicalBindingKind.BUTTON, 107),
                "options" to PhysicalBinding(PhysicalBindingKind.BUTTON, 108), "share" to PhysicalBinding(PhysicalBindingKind.BUTTON, 109),
                "dpad_up" to PhysicalBinding(PhysicalBindingKind.AXIS, AXIS_HAT_Y, AxisDirection.NEGATIVE),
                "dpad_down" to PhysicalBinding(PhysicalBindingKind.AXIS, AXIS_HAT_Y, AxisDirection.POSITIVE),
                "dpad_left" to PhysicalBinding(PhysicalBindingKind.AXIS, AXIS_HAT_X, AxisDirection.NEGATIVE),
                "dpad_right" to PhysicalBinding(PhysicalBindingKind.AXIS, AXIS_HAT_X, AxisDirection.POSITIVE),
                "ps" to PhysicalBinding(PhysicalBindingKind.BUTTON, 188),
            ),
        )

        /** Android MotionEvent axis codes for the D-Pad HAT. */
        const val AXIS_HAT_X = 15
        const val AXIS_HAT_Y = 16
    }
}
