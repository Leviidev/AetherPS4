package com.bachatas4.android.runtime.input

import kotlin.math.abs

data class ConnectedController(val deviceId: Int, val key: ControllerDeviceKey)

class ControllerBindingResolver {
    fun assignSlots(profiles: List<ControllerProfile>, devices: List<ConnectedController>): Map<Int, Int> {
        require(profiles.size <= 4) { "Only four controller slots are supported" }
        val unused = devices.toMutableList()
        return buildMap {
            profiles.forEachIndexed { slot, profile ->
                val match = unused.firstOrNull { profile.device == null || sameDevice(profile.device, it.key) }
                if (match != null) { put(slot, match.deviceId); unused.remove(match) }
            }
        }
    }

    fun snapshot(profile: ControllerProfile, values: Map<PhysicalBinding, Float>): ControllerSnapshot {
        fun value(control: String): Float {
            val binding = profile.bindings[control]
            val raw = binding?.let { values[it] } ?: 0f
            val dead = if (control.endsWith("_x") || control.endsWith("_y")) raw.takeUnless { abs(it) < profile.deadZone } ?: 0f else raw
            return if (control in profile.invertAxes) -dead else dead
        }
        fun buttonActive(control: String): Boolean {
            val binding = profile.bindings[control] ?: return false
            val v = values[binding] ?: 0f
            return when (binding.direction) {
                AxisDirection.NEGATIVE -> v <= -BUTTON_THRESHOLD
                AxisDirection.POSITIVE -> v >= BUTTON_THRESHOLD
                AxisDirection.BOTH -> v >= BUTTON_THRESHOLD
            }
        }
        var buttons = 0L
        BUTTON_BITS.forEach { (control, bit) -> if (buttonActive(control)) buttons = buttons or bit }
        val leftTrigger = value("left_trigger").coerceIn(0f, 1f)
        val rightTrigger = value("right_trigger").coerceIn(0f, 1f)
        if (leftTrigger >= profile.triggerThreshold || value("l2") >= 0.5f) buttons = buttons or Ps4Button.L2
        if (rightTrigger >= profile.triggerThreshold || value("r2") >= 0.5f) buttons = buttons or Ps4Button.R2
        return ControllerSnapshot.normalized(
            buttons = buttons,
            leftX = value("left_x"), leftY = value("left_y"), rightX = value("right_x"), rightY = value("right_y"),
            leftTrigger = leftTrigger, rightTrigger = rightTrigger,
        )
    }

    fun snapshotOrNeutral(profile: ControllerProfile, device: ControllerDeviceKey?, values: Map<PhysicalBinding, Float>): ControllerSnapshot =
        if (profile.device != null && (device == null || !sameDevice(profile.device, device))) ControllerSnapshot.Neutral else snapshot(profile, values)

    private fun sameDevice(expected: ControllerDeviceKey, actual: ControllerDeviceKey): Boolean =
        expected.descriptor == actual.descriptor && expected.vendorId == actual.vendorId && expected.productId == actual.productId

    private companion object {
        const val BUTTON_THRESHOLD = 0.5f
        val BUTTON_BITS = mapOf(
            "cross" to Ps4Button.CROSS, "circle" to Ps4Button.CIRCLE, "square" to Ps4Button.SQUARE, "triangle" to Ps4Button.TRIANGLE,
            "l1" to Ps4Button.L1, "r1" to Ps4Button.R1, "l3" to Ps4Button.L3, "r3" to Ps4Button.R3,
            "dpad_up" to Ps4Button.UP, "dpad_down" to Ps4Button.DOWN, "dpad_left" to Ps4Button.LEFT, "dpad_right" to Ps4Button.RIGHT,
            "options" to Ps4Button.OPTIONS, "touchpad" to Ps4Button.TOUCH_PAD, "ps" to Ps4Button.PS,
        )
    }
}
