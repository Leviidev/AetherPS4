package com.bachatas4.android.runtime.input

import kotlin.math.abs

object Ps4Button {
    const val L3 = 0x2L
    const val R3 = 0x4L
    const val OPTIONS = 0x8L
    const val UP = 0x10L
    const val RIGHT = 0x20L
    const val DOWN = 0x40L
    const val LEFT = 0x80L
    const val L2 = 0x100L
    const val R2 = 0x200L
    const val L1 = 0x400L
    const val R1 = 0x800L
    const val TRIANGLE = 0x1000L
    const val CIRCLE = 0x2000L
    const val CROSS = 0x4000L
    const val SQUARE = 0x8000L
    const val TOUCH_PAD = 0x100000L
    const val SHARE = 0x1L
    const val PS = 0x10000L
}

@ConsistentCopyVisibility
data class ControllerSnapshot private constructor(
    val buttons: Long,
    val leftX: Float,
    val leftY: Float,
    val rightX: Float,
    val rightY: Float,
    val leftTrigger: Float,
    val rightTrigger: Float,
    val touchDown: Boolean,
    val touchX: Float,
    val touchY: Float,
) {
    companion object {
        const val StickDeadzone = 0.08f

        val Neutral = normalized()

        fun normalized(
            buttons: Long = 0,
            leftX: Float = 0f,
            leftY: Float = 0f,
            rightX: Float = 0f,
            rightY: Float = 0f,
            leftTrigger: Float = 0f,
            rightTrigger: Float = 0f,
            touchDown: Boolean = false,
            touchX: Float = 0f,
            touchY: Float = 0f,
        ): ControllerSnapshot = ControllerSnapshot(
            buttons = buttons,
            leftX = stick(leftX),
            leftY = stick(leftY),
            rightX = stick(rightX),
            rightY = stick(rightY),
            leftTrigger = leftTrigger.coerceIn(0f, 1f),
            rightTrigger = rightTrigger.coerceIn(0f, 1f),
            touchDown = touchDown,
            touchX = touchX.coerceIn(0f, 1f),
            touchY = touchY.coerceIn(0f, 1f),
        )

        private fun stick(value: Float): Float =
            value.coerceIn(-1f, 1f).let { if (abs(it) < StickDeadzone) 0f else it }
    }
}
