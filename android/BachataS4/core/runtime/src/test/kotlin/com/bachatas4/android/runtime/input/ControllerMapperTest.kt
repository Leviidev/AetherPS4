package com.bachatas4.android.runtime.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ControllerMapperTest {
    private val mapper = ControllerMapper()

    @Test fun normalizesButtonsAndPreservesMetadata() {
        assertEquals(ControllerEvent(7, 42, "cross", 1f), mapper.button(7, 42, 96, true))
        assertNull(mapper.button(7, 42, -1, true))
    }

    @Test fun clampsAxesAndAppliesDeadzone() {
        assertEquals(0f, mapper.axis(1, 2, 0, 0.079f)!!.value)
        assertEquals(1f, mapper.axis(1, 2, 0, 4f)!!.value)
        assertEquals(-1f, mapper.axis(1, 2, 1, -4f)!!.value)
    }

    @Test fun recognizesHatAxes() {
        // AXIS_HAT_X = 15, AXIS_HAT_Y = 16 — previously omitted, caused D-Pad drop.
        assertEquals("hat_x", mapper.axis(1, 2, 15, 0.5f)!!.control)
        assertEquals("hat_y", mapper.axis(1, 2, 16, -0.5f)!!.control)
    }

    @Test fun hatAxesApplyDeadzoneAndClamp() {
        assertEquals(1f, mapper.axis(1, 2, 15, 4f)!!.value)
        assertEquals(-1f, mapper.axis(1, 2, 16, -4f)!!.value)
        assertEquals(0f, mapper.axis(1, 2, 15, 0.04f)!!.value)
    }
}
