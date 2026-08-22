package com.bachatas4.android.feature.session.controller

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchLayoutTest {
    @Test
    fun validatesSerializationAndSafeAreaMovement() {
        val layout = TouchLayout()
        val moved = TouchLayoutRenderer.move(layout, "left_stick", -1f, 2f, NormalizedInsets(0.1f, 0.1f, 0.1f, 0.1f))
        val stick = moved.controls.first { it.control == "left_stick" }
        assertTrue(stick.centerX >= 0.1f + stick.width / 2f)
        assertTrue(stick.centerY <= 0.9f - stick.height / 2f)
        assertEquals(moved, Json.decodeFromString<TouchLayout>(Json.encodeToString(moved)))
        assertThrows(IllegalArgumentException::class.java) { TouchControlPlacement("bad", Float.NaN, 0f, 0.1f, 0.1f) }
    }

    @Test
    fun hitTestUsesVisibilityAndZOrder() {
        val base = TouchControlPlacement("base", 0.5f, 0.5f, 0.2f, 0.2f, zIndex = 1)
        val top = TouchControlPlacement("top", 0.5f, 0.5f, 0.2f, 0.2f, zIndex = 2)
        val layout = TouchLayout(controls = listOf(base, top))
        assertEquals("top", TouchLayoutRenderer.hitTest(layout, 0.5f, 0.5f)?.control)
        val hidden = TouchLayoutRenderer.setVisible(layout, "top", false)
        assertEquals("base", TouchLayoutRenderer.hitTest(hidden, 0.5f, 0.5f)?.control)
        assertFalse(hidden.controls.first { it.control == "top" }.visible)
    }

    @Test
    fun resizeKeepsControlsReachable() {
        val resized = TouchLayoutRenderer.resize(TouchLayout(), "cross", 0f, 0f)
        val cross = resized.controls.first { it.control == "cross" }
        assertEquals(TouchControlPlacement.MIN_SIZE, cross.width)
        assertEquals(TouchControlPlacement.MIN_SIZE, cross.height)
    }
}
