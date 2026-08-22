package com.bachatas4.android.feature.session.controller

import com.bachatas4.android.runtime.input.ControllerSnapshot
import com.bachatas4.android.runtime.input.Ps4Button
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchControllerStateTest {
    @Test fun supportsStickAndButtonWithSeparatePointers() {
        val state = TouchControllerState()

        state.pointerDown(1, TouchControllerState.LEFT_STICK_X, TouchControllerState.STICK_Y)
        state.pointerMove(
            1,
            TouchControllerState.LEFT_STICK_X + TouchControllerState.STICK_RADIUS,
            TouchControllerState.STICK_Y,
        )
        val cross = TouchControllerState.faceButtons.first { it.button == Ps4Button.CROSS }
        state.pointerDown(2, cross.x, cross.y)
        val snapshot = state.snapshot()

        assertEquals(1f, snapshot.leftX)
        assertEquals(0f, snapshot.leftY)
        assertTrue(snapshot.buttons and Ps4Button.CROSS != 0L)
        state.pointerUp(2)
        assertEquals(0L, state.snapshot().buttons and Ps4Button.CROSS)
        assertEquals(1f, state.snapshot().leftX)
    }

    @Test fun slidingFaceButtonUpdatesPressedButton() {
        val state = TouchControllerState()
        val cross = TouchControllerState.faceButtons.first { it.button == Ps4Button.CROSS }
        val circle = TouchControllerState.faceButtons.first { it.button == Ps4Button.CIRCLE }
        state.pointerDown(1, cross.x, cross.y)
        state.pointerMove(1, circle.x, circle.y)

        assertEquals(Ps4Button.CIRCLE, state.snapshot().buttons)
    }

    @Test fun cancelAllReturnsNeutralSnapshot() {
        val state = TouchControllerState()
        state.pointerDown(1, TouchControllerState.LEFT_STICK_X, TouchControllerState.STICK_Y)
        val triangle = TouchControllerState.faceButtons.first { it.button == Ps4Button.TRIANGLE }
        state.pointerDown(2, triangle.x, triangle.y)
        state.cancelAll()

        assertEquals(ControllerSnapshot.Neutral, state.snapshot())
    }

    @Test fun everyFaceButtonCenterResolvesWithoutStickInterception() {
        TouchControllerState.faceButtons.forEachIndexed { index, control ->
            val state = TouchControllerState()
            state.pointerDown(index.toLong(), control.x, control.y)
            assertEquals(control.button, state.snapshot().buttons)
        }
    }

    @Test fun touchpadAndOptionsRemainMappedAfterTheVisualRedesign() {
        val state = TouchControllerState()
        TouchLayout.defaultControls()
            .filter { it.control in setOf("touchpad", "options") }
            .forEachIndexed { index, placement ->
                state.pointerDown(index.toLong(), placement.centerX * 1920f, placement.centerY * 1080f)
            }

        assertTrue(state.snapshot().buttons and Ps4Button.TOUCH_PAD != 0L)
        assertTrue(state.snapshot().buttons and Ps4Button.OPTIONS != 0L)
    }
}
