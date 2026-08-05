package com.bachatas4.android.data

import android.content.pm.ActivityInfo
import org.junit.Assert.assertEquals
import org.junit.Test

class UiOrientationPreferenceTest {
    @Test
    fun decodeDefaultsWhenMissingOrInvalid() {
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode(null))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode(""))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("   "))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("unknown"))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("PORTRAIT"))
    }

    @Test
    fun decodeAcceptsStoredValues() {
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.decode("portrait"))
        assertEquals(UiOrientation.Landscape, UiOrientationPreference.decode("landscape"))
    }

    @Test
    fun encodeIsStableLowercase() {
        assertEquals("portrait", UiOrientationPreference.encode(UiOrientation.Portrait))
        assertEquals("landscape", UiOrientationPreference.encode(UiOrientation.Landscape))
    }

    @Test
    fun mapsToSensorAxisLocks() {
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT,
            UiOrientationPreference.toActivityOrientation(UiOrientation.Portrait),
        )
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
            UiOrientationPreference.toActivityOrientation(UiOrientation.Landscape),
        )
    }

    @Test
    fun toggleFlipsBothWays() {
        assertEquals(UiOrientation.Landscape, UiOrientationPreference.toggle(UiOrientation.Portrait))
        assertEquals(UiOrientation.Portrait, UiOrientationPreference.toggle(UiOrientation.Landscape))
    }
}
