package com.bachatas4.android.designsystem.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

object BachataPalette {
    val Canvas = Color(0xFF0A0A0C)
    val Surface = Color(0xFF1C1C20)
    val RaisedSurface = Color(0xFF26262B)
    val Primary = Color(0xFFF5F4F1)
    val Secondary = Color(0xFF8B8D94)
    val Accent = Color(0xFFE8A33D)
    val OnAccent = Color(0xFF1A1206)

    /** Status banner colors — replaces ad-hoc 0xFF literals scattered across screens. */
    val Info = Color(0xFF1E3A5F)
    val InfoText = Color(0xFFD2E8FF)
    val InfoSubtle = Color(0xFF8AB4F8)
    val Warning = Color(0xFF3A2B18)
    val WarningText = Color(0xFFFFDDB5)
    val Success = Color(0xFF213D21)
    val SuccessText = Color(0xFFB5E8B5)
    val Error = Color(0xFF3D2121)
    val ErrorText = Color(0xFFE8B5B5)
}

private val BachataColors = darkColorScheme(
    primary = BachataPalette.Accent,
    onPrimary = BachataPalette.OnAccent,
    background = BachataPalette.Canvas,
    surface = BachataPalette.Surface,
    surfaceVariant = BachataPalette.RaisedSurface,
    onBackground = BachataPalette.Primary,
    onSurface = BachataPalette.Primary,
    onSurfaceVariant = BachataPalette.Secondary,
)

@Composable
fun AppTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit,
) {
    MaterialTheme(colorScheme = BachataColors, content = content)
}
