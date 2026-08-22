package com.bachatas4.android.feature.session.controller

import org.junit.Assert.assertNotEquals
import org.junit.Test

class TouchControlVisualStyleTest {
    @Test
    fun mapsEveryDefaultControlToAVisualStyle() {
        TouchLayout.defaultControls().forEach { placement ->
            assertNotEquals(
                TouchControlVisualStyle.Unknown,
                TouchControlVisualStyle.forControl(placement.control),
            )
        }
    }
}
