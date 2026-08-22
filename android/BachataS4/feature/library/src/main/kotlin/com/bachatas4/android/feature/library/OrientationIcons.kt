package com.bachatas4.android.feature.library

import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp
import com.bachatas4.android.data.UiOrientation
import com.bachatas4.android.designsystem.theme.BachataPalette
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * Console-style Library header glyphs inspired by GameHub chrome:
 * - Settings: hexagon frame with a clean center gear
 * - Orientation: rounded phone + dual rotate arcs (portrait / landscape)
 */
internal object LibraryHeaderIcons {
    val Settings: ImageVector by lazy {
        ImageVector.Builder(
            name = "LibraryHexSettings",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f,
        ).apply {
            // Hexagon outline (ring)
            path(
                fill = SolidColor(Color.Black),
                pathFillType = PathFillType.EvenOdd,
            ) {
                hexagon(cx = 12f, cy = 12f, r = 10.2f)
                hexagon(cx = 12f, cy = 12f, r = 8.35f)
            }
            // Gear (6 teeth) + hub hole
            path(
                fill = SolidColor(Color.Black),
                pathFillType = PathFillType.EvenOdd,
            ) {
                gear(cx = 12f, cy = 12f, outerR = 4.55f, innerR = 3.35f, teeth = 6, toothHalf = 0.42f)
                // hub hole
                circle(cx = 12f, cy = 12f, r = 1.55f)
            }
        }.build()
    }

    val Portrait: ImageVector by lazy {
        phoneWithArcs(
            name = "LibraryPortraitRotate",
            phoneLeft = 8.35f,
            phoneTop = 3.55f,
            phoneWidth = 7.3f,
            phoneHeight = 12.9f,
            arcA = ArcSpec(cx = 12f, cy = 10.2f, r = 8.9f, startDeg = -48f, sweepDeg = 62f),
            arcB = ArcSpec(cx = 12f, cy = 13.8f, r = 8.9f, startDeg = 132f, sweepDeg = 62f),
        )
    }

    val Landscape: ImageVector by lazy {
        phoneWithArcs(
            name = "LibraryLandscapeRotate",
            phoneLeft = 3.55f,
            phoneTop = 8.35f,
            phoneWidth = 12.9f,
            phoneHeight = 7.3f,
            arcA = ArcSpec(cx = 13.8f, cy = 12f, r = 8.9f, startDeg = 42f, sweepDeg = 62f),
            arcB = ArcSpec(cx = 10.2f, cy = 12f, r = 8.9f, startDeg = 222f, sweepDeg = 62f),
        )
    }

    private data class ArcSpec(
        val cx: Float,
        val cy: Float,
        val r: Float,
        val startDeg: Float,
        val sweepDeg: Float,
    )

    private fun phoneWithArcs(
        name: String,
        phoneLeft: Float,
        phoneTop: Float,
        phoneWidth: Float,
        phoneHeight: Float,
        arcA: ArcSpec,
        arcB: ArcSpec,
    ): ImageVector = ImageVector.Builder(
        name = name,
        defaultWidth = 24.dp,
        defaultHeight = 24.dp,
        viewportWidth = 24f,
        viewportHeight = 24f,
    ).apply {
        val corner = 1.45f
        val inset = 1.4f
        path(
            fill = SolidColor(Color.Black),
            pathFillType = PathFillType.EvenOdd,
        ) {
            roundedRect(phoneLeft, phoneTop, phoneWidth, phoneHeight, corner)
            roundedRect(
                phoneLeft + inset,
                phoneTop + inset,
                phoneWidth - inset * 2f,
                phoneHeight - inset * 2f,
                corner * 0.55f,
            )
        }
        // Home indicator
        val bar = min(phoneWidth, phoneHeight) * 0.26f
        val barThick = 0.85f
        path(fill = SolidColor(Color.Black)) {
            if (phoneHeight > phoneWidth) {
                val bx = phoneLeft + (phoneWidth - bar) / 2f
                val by = phoneTop + phoneHeight - inset - 1.55f
                roundedRect(bx, by, bar, barThick, barThick / 2f)
            } else {
                val bx = phoneLeft + phoneWidth - inset - 1.55f
                val by = phoneTop + (phoneHeight - bar) / 2f
                roundedRect(bx, by, barThick, bar, barThick / 2f)
            }
        }
        drawArcWithTip(arcA)
        drawArcWithTip(arcB)
    }.build()

    private fun PathBuilder.roundedRect(
        left: Float,
        top: Float,
        width: Float,
        height: Float,
        radius: Float,
    ) {
        val right = left + width
        val bottom = top + height
        val rr = radius.coerceAtMost(min(width, height) / 2f)
        moveTo(left + rr, top)
        lineTo(right - rr, top)
        arcTo(rr, rr, 0f, false, true, right, top + rr)
        lineTo(right, bottom - rr)
        arcTo(rr, rr, 0f, false, true, right - rr, bottom)
        lineTo(left + rr, bottom)
        arcTo(rr, rr, 0f, false, true, left, bottom - rr)
        lineTo(left, top + rr)
        arcTo(rr, rr, 0f, false, true, left + rr, top)
        close()
    }

    private fun PathBuilder.hexagon(cx: Float, cy: Float, r: Float) {
        // Pointy-top hexagon
        for (i in 0 until 6) {
            val angle = Math.toRadians((-90.0 + i * 60.0))
            val x = cx + r * cos(angle).toFloat()
            val y = cy + r * sin(angle).toFloat()
            if (i == 0) moveTo(x, y) else lineTo(x, y)
        }
        close()
    }

    private fun PathBuilder.circle(cx: Float, cy: Float, r: Float) {
        moveTo(cx + r, cy)
        arcTo(r, r, 0f, false, true, cx - r, cy)
        arcTo(r, r, 0f, false, true, cx + r, cy)
        close()
    }

    private fun PathBuilder.gear(
        cx: Float,
        cy: Float,
        outerR: Float,
        innerR: Float,
        teeth: Int,
        toothHalf: Float,
    ) {
        val step = 360f / teeth
        // Build a star-like gear outline alternating outer tips and inner valleys
        var started = false
        for (i in 0 until teeth) {
            val base = -90f + i * step
            val a0 = Math.toRadians((base - toothHalf * 22f).toDouble())
            val a1 = Math.toRadians((base - toothHalf * 8f).toDouble())
            val a2 = Math.toRadians((base + toothHalf * 8f).toDouble())
            val a3 = Math.toRadians((base + toothHalf * 22f).toDouble())
            val points = listOf(
                cx + innerR * cos(a0).toFloat() to cy + innerR * sin(a0).toFloat(),
                cx + outerR * cos(a1).toFloat() to cy + outerR * sin(a1).toFloat(),
                cx + outerR * cos(a2).toFloat() to cy + outerR * sin(a2).toFloat(),
                cx + innerR * cos(a3).toFloat() to cy + innerR * sin(a3).toFloat(),
            )
            for ((x, y) in points) {
                if (!started) {
                    moveTo(x, y)
                    started = true
                } else {
                    lineTo(x, y)
                }
            }
        }
        close()
    }

    private fun ImageVector.Builder.drawArcWithTip(spec: ArcSpec) {
        val startRad = Math.toRadians(spec.startDeg.toDouble())
        val endRad = Math.toRadians((spec.startDeg + spec.sweepDeg).toDouble())
        val x1 = spec.cx + spec.r * cos(startRad).toFloat()
        val y1 = spec.cy + spec.r * sin(startRad).toFloat()
        val x2 = spec.cx + spec.r * cos(endRad).toFloat()
        val y2 = spec.cy + spec.r * sin(endRad).toFloat()
        val large = kotlin.math.abs(spec.sweepDeg) > 180f
        val sweepPositive = spec.sweepDeg > 0f

        path(
            fill = null,
            stroke = SolidColor(Color.Black),
            strokeLineWidth = 1.6f,
            strokeLineCap = StrokeCap.Round,
            strokeLineJoin = StrokeJoin.Round,
        ) {
            moveTo(x1, y1)
            arcToRelative(
                a = spec.r,
                b = spec.r,
                theta = 0f,
                isMoreThanHalf = large,
                isPositiveArc = sweepPositive,
                dx1 = x2 - x1,
                dy1 = y2 - y1,
            )
        }

        val tangent = endRad + if (sweepPositive) Math.PI / 2.0 else -Math.PI / 2.0
        val len = 2.15f
        val half = 1.3f
        val bx = x2 - len * cos(tangent).toFloat()
        val by = y2 - len * sin(tangent).toFloat()
        val px = half * cos(tangent + Math.PI / 2).toFloat()
        val py = half * sin(tangent + Math.PI / 2).toFloat()
        path(fill = SolidColor(Color.Black)) {
            moveTo(x2, y2)
            lineTo(bx + px, by + py)
            lineTo(bx - px, by - py)
            close()
        }
    }
}

/** Back-compat aliases for orientation toggle. */
internal object OrientationIcons {
    val Portrait: ImageVector get() = LibraryHeaderIcons.Portrait
    val Landscape: ImageVector get() = LibraryHeaderIcons.Landscape
}

@Composable
internal fun OrientationToggleButton(
    orientation: UiOrientation,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val icon = remember(orientation) {
        if (orientation == UiOrientation.Portrait) {
            LibraryHeaderIcons.Portrait
        } else {
            LibraryHeaderIcons.Landscape
        }
    }
    val description = if (orientation == UiOrientation.Portrait) {
        "Switch to landscape"
    } else {
        "Switch to portrait"
    }
    IconButton(onClick = onClick, modifier = modifier.size(40.dp)) {
        Icon(
            imageVector = icon,
            contentDescription = description,
            tint = BachataPalette.Primary,
            modifier = Modifier.size(24.dp),
        )
    }
}

@Composable
internal fun LibrarySettingsButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    IconButton(onClick = onClick, modifier = modifier.size(40.dp)) {
        Icon(
            imageVector = LibraryHeaderIcons.Settings,
            contentDescription = "Settings",
            tint = BachataPalette.Primary,
            modifier = Modifier.size(22.dp),
        )
    }
}
