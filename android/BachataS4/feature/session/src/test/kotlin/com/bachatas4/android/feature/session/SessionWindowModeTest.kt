package com.bachatas4.android.feature.session

import android.content.pm.ActivityInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionWindowModeTest {
    @Test
    fun gameplayIsFixedLandscapeAndImmersive() {
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, SessionWindowMode.ImmersiveLandscape.orientation)
        assertTrue(SessionWindowMode.ImmersiveLandscape.hideSystemBars)
    }

    @Test
    fun normalRoutesUseSensorPortraitAndVisibleBars() {
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT, SessionWindowMode.Portrait.orientation)
        assertFalse(SessionWindowMode.Portrait.hideSystemBars)
    }
}
