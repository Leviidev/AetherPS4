package com.bachatas4.android.runtime.display

import org.junit.Assert.assertEquals
import org.junit.Test

class WindowScalingTest {
    @Test
    fun centersLandscapeGameAtAspectFitSize() {
        assertEquals(
            FloatBounds(left = 0f, top = 100f, right = 1920f, bottom = 1180f),
            aspectFitBounds(sourceWidth = 1280, sourceHeight = 720, targetWidth = 1920, targetHeight = 1280),
        )
    }
}
