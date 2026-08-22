package com.bachatas4.android.feature.session.controller

import com.bachatas4.android.runtime.input.ControllerSnapshot
import com.bachatas4.android.runtime.input.Ps4Button
import kotlin.math.hypot

class TouchControllerState(private val layout: TouchLayout = TouchLayout()) {
    data class ButtonControl(val x: Float, val y: Float, val button: Long, val label: String)

    private sealed interface ActiveControl {
        data class Stick(
            val right: Boolean,
            val centerX: Float,
            val centerY: Float,
            val radiusX: Float,
            val radiusY: Float,
            var x: Float = 0f,
            var y: Float = 0f,
        ) : ActiveControl
        data class Button(var mask: Long) : ActiveControl
    }

    private val pointers = linkedMapOf<Long, ActiveControl>()

    fun pointerDown(id: Long, x: Float, y: Float) {
        val placement = hit(x, y)
        val control = if (placement?.control == "left_stick" || placement?.control == "right_stick") {
            val centerX = if (layout.analogCentering) placement.centerX * LogicalWidth else x
            val centerY = if (layout.analogCentering) placement.centerY * LogicalHeight else y
            ActiveControl.Stick(
                right = placement.control == "right_stick",
                centerX = centerX,
                centerY = centerY,
                radiusX = placement.width * layout.scale * LogicalWidth / 2f,
                radiusY = placement.height * layout.scale * LogicalHeight / 2f,
            )
        } else ActiveControl.Button(button(placement?.control))
        pointers[id] = control
        pointerMove(id, x, y)
    }

    fun pointerMove(id: Long, x: Float, y: Float) {
        when (val control = pointers[id]) {
            is ActiveControl.Stick -> {
                var dx = (x - control.centerX) / control.radiusX.coerceAtLeast(1f)
                var dy = (y - control.centerY) / control.radiusY.coerceAtLeast(1f)
                val length = hypot(dx, dy)
                if (length > 1f) { dx /= length; dy /= length }
                control.x = dx
                control.y = dy
            }
            is ActiveControl.Button -> control.mask = button(hit(x, y)?.control)
            null -> Unit
        }
    }

    fun pointerUp(id: Long) { pointers.remove(id) }
    fun cancelAll() { pointers.clear() }

    fun snapshot(): ControllerSnapshot {
        var buttons = 0L
        var leftX = 0f; var leftY = 0f; var rightX = 0f; var rightY = 0f
        pointers.values.forEach { control ->
            when (control) {
                is ActiveControl.Button -> buttons = buttons or control.mask
                is ActiveControl.Stick -> if (control.right) { rightX = control.x; rightY = control.y } else { leftX = control.x; leftY = control.y }
            }
        }
        return ControllerSnapshot.normalized(
            buttons = buttons, leftX = leftX, leftY = leftY, rightX = rightX, rightY = rightY,
            leftTrigger = if (buttons and Ps4Button.L2 != 0L) 1f else 0f,
            rightTrigger = if (buttons and Ps4Button.R2 != 0L) 1f else 0f,
        )
    }

    private fun hit(x: Float, y: Float) = TouchLayoutRenderer.hitTest(layout, x / LogicalWidth, y / LogicalHeight)
    private fun button(control: String?): Long = BUTTONS[control] ?: 0L

    companion object {
        const val LogicalWidth = 1920f
        const val LogicalHeight = 1080f
        const val LEFT_STICK_X = 410f
        const val RIGHT_STICK_X = 1510f
        const val STICK_Y = 880f
        const val STICK_RADIUS = 100f
        const val BUTTON_RADIUS = 55f
        const val DPAD_RADIUS = 55f
        private val BUTTONS = mapOf(
            "triangle" to Ps4Button.TRIANGLE, "circle" to Ps4Button.CIRCLE, "cross" to Ps4Button.CROSS, "square" to Ps4Button.SQUARE,
            "dpad_up" to Ps4Button.UP, "dpad_right" to Ps4Button.RIGHT, "dpad_down" to Ps4Button.DOWN, "dpad_left" to Ps4Button.LEFT,
            "l1" to Ps4Button.L1, "l2" to Ps4Button.L2, "r1" to Ps4Button.R1, "r2" to Ps4Button.R2,
            "touchpad" to Ps4Button.TOUCH_PAD, "options" to Ps4Button.OPTIONS,
            "share" to Ps4Button.SHARE, "ps" to Ps4Button.PS,
        )
        val faceButtons = listOf(
            ButtonControl(1700f, 570f, Ps4Button.TRIANGLE, "TRI"), ButtonControl(1820f, 690f, Ps4Button.CIRCLE, "CIR"),
            ButtonControl(1700f, 810f, Ps4Button.CROSS, "X"), ButtonControl(1580f, 690f, Ps4Button.SQUARE, "SQ"),
        )
        val dpadButtons = listOf(
            ButtonControl(220f, 580f, Ps4Button.UP, "UP"), ButtonControl(330f, 690f, Ps4Button.RIGHT, "R"),
            ButtonControl(220f, 800f, Ps4Button.DOWN, "DN"), ButtonControl(110f, 690f, Ps4Button.LEFT, "L"),
        )
    }
}
