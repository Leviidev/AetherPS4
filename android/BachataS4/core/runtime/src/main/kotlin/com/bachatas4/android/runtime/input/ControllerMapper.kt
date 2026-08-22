package com.bachatas4.android.runtime.input

import kotlin.math.abs

data class ControllerEvent(
    val deviceId: Int,
    val timestampMs: Long,
    val control: String,
    val value: Float,
)

data class PhysicalControllerEvent(
    val deviceId: Int,
    val device: ControllerDeviceKey,
    val binding: PhysicalBinding,
    val value: Float,
)

class ControllerMapper(
    private val deadzone: Float = DEFAULT_DEADZONE,
) {
    init { require(deadzone in 0f..1f) }

    fun button(deviceId: Int, timestampMs: Long, keyCode: Int, pressed: Boolean): ControllerEvent? {
        val control = BUTTONS[keyCode] ?: return null
        return ControllerEvent(deviceId, timestampMs, control, if (pressed) 1f else 0f)
    }

    fun axis(deviceId: Int, timestampMs: Long, axis: Int, rawValue: Float): ControllerEvent? {
        val control = AXES[axis] ?: return null
        val clamped = rawValue.coerceIn(-1f, 1f)
        val normalized = if (abs(clamped) < deadzone) 0f else clamped
        return ControllerEvent(deviceId, timestampMs, control, normalized)
    }

    fun physicalButton(deviceId: Int, device: ControllerDeviceKey, keyCode: Int, pressed: Boolean): PhysicalControllerEvent =
        PhysicalControllerEvent(deviceId, device, PhysicalBinding(PhysicalBindingKind.BUTTON, keyCode), if (pressed) 1f else 0f)

    fun physicalAxis(deviceId: Int, device: ControllerDeviceKey, axis: Int, rawValue: Float): PhysicalControllerEvent =
        PhysicalControllerEvent(deviceId, device, PhysicalBinding(PhysicalBindingKind.AXIS, axis), rawValue.coerceIn(-1f, 1f))

    companion object {
        const val DEFAULT_DEADZONE = 0.08f
        private val BUTTONS = mapOf(
            96 to "cross", 97 to "circle", 99 to "square", 100 to "triangle",
            102 to "l1", 103 to "r1", 104 to "l2", 105 to "r2",
            106 to "l3", 107 to "r3", 108 to "options", 109 to "share",
            19 to "dpad_up", 20 to "dpad_down", 21 to "dpad_left", 22 to "dpad_right",
            188 to "ps",
        )
        private val AXES = mapOf(
            0 to "left_x", 1 to "left_y", 11 to "right_x", 14 to "right_y",
            17 to "left_trigger", 18 to "right_trigger",
            15 to "hat_x", 16 to "hat_y",
        )
    }
}
