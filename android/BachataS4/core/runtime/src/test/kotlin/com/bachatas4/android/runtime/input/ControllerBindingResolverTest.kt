package com.bachatas4.android.runtime.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerBindingResolverTest {
    private val key = ControllerDeviceKey("stable", 1, 2, "Pad")
    private val resolver = ControllerBindingResolver()

    @Test
    fun stableIdentityIgnoresTransientAndroidDeviceId() {
        val profile = ControllerProfile.standard(key)
        assertEquals(mapOf(0 to 99), resolver.assignSlots(listOf(profile), listOf(ConnectedController(99, key))))
    }

    @Test
    fun appliesDeadZoneInversionTriggersAndDisconnectNeutralization() {
        val axis = PhysicalBinding(PhysicalBindingKind.AXIS, 0)
        val trigger = PhysicalBinding(PhysicalBindingKind.AXIS, 17)
        val profile = ControllerProfile(
            device = key,
            bindings = mapOf("left_x" to axis, "left_trigger" to trigger),
            deadZone = 0.1f,
            invertAxes = setOf("left_x"),
            triggerThreshold = 0.4f,
        )
        val snapshot = resolver.snapshot(profile, mapOf(axis to 0.5f, trigger to 0.6f))
        assertEquals(-0.5f, snapshot.leftX)
        assertEquals(0.6f, snapshot.leftTrigger)
        assertTrue(snapshot.buttons and Ps4Button.L2 != 0L)
        assertEquals(ControllerSnapshot.Neutral, resolver.snapshotOrNeutral(profile, null, emptyMap()))
    }

    @Test
    fun limitsProfilesToFourSlots() {
        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            resolver.assignSlots(List(5) { ControllerProfile() }, emptyList())
        }
    }

    @Test
    fun hatNegativeDirectionMapsToDpadUp() {
        val hatY = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_up" to hatY))
        val snapshot = resolver.snapshot(profile, mapOf(hatY to -1f))
        assertTrue(snapshot.buttons and Ps4Button.UP != 0L)
    }

    @Test
    fun hatPositiveDirectionMapsToDpadDown() {
        val hatY = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.POSITIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_down" to hatY))
        val snapshot = resolver.snapshot(profile, mapOf(hatY to 1f))
        assertTrue(snapshot.buttons and Ps4Button.DOWN != 0L)
    }

    @Test
    fun hatDirectionDoesNotFireOnWrongSign() {
        val hatYNeg = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE)
        val profile = ControllerProfile(device = null, bindings = mapOf("dpad_up" to hatYNeg))
        // Positive value on a NEGATIVE-direction binding should NOT activate the button.
        val snapshot = resolver.snapshot(profile, mapOf(hatYNeg to 1f))
        assertTrue(snapshot.buttons and Ps4Button.UP == 0L)
    }

    @Test
    fun bothDirectionUsesPositiveThresholdOnly() {
        val axis = PhysicalBinding(PhysicalBindingKind.AXIS, 0, AxisDirection.BOTH)
        val profile = ControllerProfile(device = null, bindings = mapOf("cross" to axis))
        // Positive fires
        assertTrue(resolver.snapshot(profile, mapOf(axis to 0.8f)).buttons and Ps4Button.CROSS != 0L)
        // Negative does not (BOTH = legacy behavior, only checks >= threshold)
        assertTrue(resolver.snapshot(profile, mapOf(axis to -0.8f)).buttons and Ps4Button.CROSS == 0L)
    }
}
