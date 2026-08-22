package com.bachatas4.android

import org.junit.Assert.assertEquals
import org.junit.Test

class MainActivityRouteTest {
    @Test
    fun installedRuntimeStartsInLibrary() {
        assertEquals(BachataRoutes.Library, initialRouteForSoc("SM8650", isRuntimeInstalled = true))
        assertEquals(BachataRoutes.Library, initialRouteForSoc("sm8750", isRuntimeInstalled = true))
    }

    @Test
    fun missingRuntimeStartsInSetup() {
        assertEquals(BachataRoutes.Setup, initialRouteForSoc("Tensor G5", isRuntimeInstalled = false))
    }

    @Test
    fun exposesGlobalGameDriverAndEditorRoutes() {
        assertEquals("settings", BachataRoutes.Settings)
        assertEquals("settings/game/CUSA00001", BachataRoutes.gameSettings("CUSA00001"))
        assertEquals("drivers", BachataRoutes.Drivers)
    }
}
