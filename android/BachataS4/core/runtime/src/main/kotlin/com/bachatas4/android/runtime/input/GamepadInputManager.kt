package com.bachatas4.android.runtime.input

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.bachatas4.android.runtime.session.ManagedSession
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.abs

data class NavControllerEvent(
    val control: String,
    val pressed: Boolean,
)

object GamepadInputManager {
    private val mapper = ControllerMapper()
    private val resolver = ControllerBindingResolver()
    private val profile = AtomicReference(ControllerProfile.standard())
    private val perDeviceState = HashMap<Int, MutableMap<PhysicalBinding, Float>>()
    private val captureSink = AtomicReference<((PhysicalBinding) -> Unit)?>(null)
    private val navSink = AtomicReference<((NavControllerEvent) -> Boolean)?>(null)

    /** Android axis codes polled from MotionEvent. */
    private val AXIS_CODES = intArrayOf(
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y,
        MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
        MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
    )

    /** D-Pad keycodes corresponding to PhysicalBinding(BUTTON, code) in the standard profile. */
    private const val KEYCODE_DPAD_UP = 19
    private const val KEYCODE_DPAD_DOWN = 20
    private const val KEYCODE_DPAD_LEFT = 21
    private const val KEYCODE_DPAD_RIGHT = 22

    @Volatile
    var hasPhysicalController: Boolean = false
        private set

    /** Update the active controller profile at runtime (called when settings change). */
    fun setProfile(p: ControllerProfile) { profile.set(p) }

    fun registerCaptureListener(listener: (PhysicalBinding) -> Unit) {
        captureSink.set(listener)
    }

    fun unregisterCaptureListener() {
        captureSink.set(null)
    }

    fun registerNavListener(listener: (NavControllerEvent) -> Boolean) {
        navSink.set(listener)
    }

    fun unregisterNavListener() {
        navSink.set(null)
    }

    fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val deviceId = event.deviceId
        if (!isGameController(deviceId)) return false

        val capture = captureSink.get()
        if (capture != null) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                val device = InputDevice.getDevice(deviceId)
                val key = device?.let { ControllerDeviceKey(it.descriptor, it.vendorId, it.productId, it.name) }
                    ?: ControllerDeviceKey("", 0, 0, "")
                capture(mapper.physicalButton(deviceId, key, event.keyCode, true).binding)
            }
            return true
        }

        if (event.repeatCount > 0) return true

        val controlEvent = mapper.button(
            deviceId,
            event.eventTime,
            event.keyCode,
            event.action == KeyEvent.ACTION_DOWN,
        ) ?: return false

        if (event.action == KeyEvent.ACTION_DOWN) {
            val nav = navSink.get()
            if (nav != null && nav(NavControllerEvent(controlEvent.control, true))) return true
        }

        val activeProfile = profile.get()
        val binding = activeProfile.bindings[controlEvent.control] ?: return false
        val state = perDeviceState.getOrPut(deviceId) { HashMap() }
        state[binding] = controlEvent.value
        submitFromDevice(deviceId, state)
        return true
    }

    fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val deviceId = event.deviceId
        if (!isGameController(deviceId)) return false

        val capture = captureSink.get()
        if (capture != null) {
            val device = InputDevice.getDevice(deviceId)
            val key = device?.let { ControllerDeviceKey(it.descriptor, it.vendorId, it.productId, it.name) }
                ?: ControllerDeviceKey("", 0, 0, "")
            for (axis in AXIS_CODES) {
                val raw = event.getAxisValue(axis)
                if (abs(raw) >= AXIS_CAPTURE_THRESHOLD) {
                    val direction = if (raw >= AXIS_CAPTURE_THRESHOLD) AxisDirection.POSITIVE else AxisDirection.NEGATIVE
                    val binding = PhysicalBinding(PhysicalBindingKind.AXIS, axis, direction)
                    capture(binding)
                    return true
                }
            }
            return false
        }

        val nav = navSink.get()
        if (nav != null) {
            // HAT axes → nav controls via synthetic button conversion
            val hatControls = hatToNavControls(event)
            for (hc in hatControls) {
                if (nav(NavControllerEvent(hc.first, hc.second))) return true
            }
            for (axis in AXIS_CODES) {
                if (axis == MotionEvent.AXIS_HAT_X || axis == MotionEvent.AXIS_HAT_Y) continue // handled above
                val raw = event.getAxisValue(axis)
                val controlEvent = mapper.axis(deviceId, event.eventTime, axis, raw) ?: continue
                val navEvent = NavControllerEvent(controlEvent.control, abs(controlEvent.value) >= AXIS_CAPTURE_THRESHOLD)
                if (nav(navEvent)) return true
            }
            return false
        }

        val activeProfile = profile.get()
        val state = perDeviceState.getOrPut(deviceId) { HashMap() }
        var handled = false

        // Part A: synthetic HAT→button conversion for profiles that bind dpad as BUTTON keycodes.
        // Works with the standard profile out-of-the-box — no remapping required.
        val synthBindings = setOf(
            activeProfile.bindings["dpad_up"],
            activeProfile.bindings["dpad_down"],
            activeProfile.bindings["dpad_left"],
            activeProfile.bindings["dpad_right"],
        )
        val usesSyntheticDpad = synthBindings.any { it != null && it.kind == PhysicalBindingKind.BUTTON }
        if (usesSyntheticDpad) {
            applySyntheticHat(event, activeProfile, state)
            handled = true
        }

        // Part B3: profile-driven axis processing with direction awareness.
        for (axis in AXIS_CODES) {
            val raw = event.getAxisValue(axis)
            val controlEvent = mapper.axis(deviceId, event.eventTime, axis, raw) ?: continue
            val binding = activeProfile.bindings[controlEvent.control] ?: continue
            state[binding] = controlEvent.value
            handled = true
        }

        if (handled) {
            submitFromDevice(deviceId, state)
        }
        return handled
    }

    /**
     * Convert HAT axis values to synthetic D-Pad button states for profiles that map the D-Pad to
     * keycodes 19-22 (the default [ControllerProfile.standard] mapping). This lets HAT-based
     * controllers (most Xbox-style gamepads) work without any remapping.
     */
    private fun applySyntheticHat(
        event: MotionEvent,
        activeProfile: ControllerProfile,
        state: MutableMap<PhysicalBinding, Float>,
    ) {
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        activeProfile.bindings["dpad_up"]?.let { b -> state[b] = if (hatY <= -HAT_THRESHOLD) 1f else 0f }
        activeProfile.bindings["dpad_down"]?.let { b -> state[b] = if (hatY >= HAT_THRESHOLD) 1f else 0f }
        activeProfile.bindings["dpad_left"]?.let { b -> state[b] = if (hatX <= -HAT_THRESHOLD) 1f else 0f }
        activeProfile.bindings["dpad_right"]?.let { b -> state[b] = if (hatX >= HAT_THRESHOLD) 1f else 0f }
    }

    /** HAT → (navControl, pressed) pairs for the nav listener path. */
    private fun hatToNavControls(event: MotionEvent): List<Pair<String, Boolean>> {
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        return listOf(
            "dpad_up" to (hatY <= -HAT_THRESHOLD),
            "dpad_down" to (hatY >= HAT_THRESHOLD),
            "dpad_left" to (hatX <= -HAT_THRESHOLD),
            "dpad_right" to (hatX >= HAT_THRESHOLD),
        )
    }

    /** Detect whether a device exposes HAT axes (Xbox-style D-Pad). */
    fun hasHatDpad(deviceId: Int): Boolean {
        val device = InputDevice.getDevice(deviceId) ?: return false
        val ranges = device.motionRanges ?: return false
        return ranges.any { it.axis == MotionEvent.AXIS_HAT_X || it.axis == MotionEvent.AXIS_HAT_Y }
    }

    fun onSessionStart() {
        perDeviceState.clear()
        hasPhysicalController = false
    }

    fun onSessionEnd() {
        perDeviceState.clear()
        hasPhysicalController = false
    }

    private fun submitFromDevice(deviceId: Int, state: Map<PhysicalBinding, Float>) {
        hasPhysicalController = perDeviceState.isNotEmpty()
        val snapshot = resolver.snapshot(profile.get(), state)
        ManagedSession.submitController(snapshot)
    }

    private fun isGameController(deviceId: Int): Boolean {
        val device = InputDevice.getDevice(deviceId) ?: return false
        if (device.isVirtual) return false
        val name = device.name ?: ""
        if (name.contains("uinput-fpc") || name.contains("goodix_fp") || name.startsWith("uinput-")) return false
        val sources = device.sources
        return (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    private const val AXIS_CAPTURE_THRESHOLD = 0.5f
    private const val HAT_THRESHOLD = 0.5f
}
