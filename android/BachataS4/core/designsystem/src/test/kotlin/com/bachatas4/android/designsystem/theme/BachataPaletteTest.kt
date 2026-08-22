package com.bachatas4.android.designsystem.theme

import androidx.compose.ui.graphics.Color
import org.junit.Assert.assertEquals
import org.junit.Test

class BachataPaletteTest {
    @Test
    fun usesApprovedStageColors() {
        assertEquals(Color(0xFF0A0A0C), BachataPalette.Canvas)
        assertEquals(Color(0xFF1C1C20), BachataPalette.Surface)
        assertEquals(Color(0xFF26262B), BachataPalette.RaisedSurface)
        assertEquals(Color(0xFFF5F4F1), BachataPalette.Primary)
        assertEquals(Color(0xFF8B8D94), BachataPalette.Secondary)
        assertEquals(Color(0xFFE8A33D), BachataPalette.Accent)
    }
}
