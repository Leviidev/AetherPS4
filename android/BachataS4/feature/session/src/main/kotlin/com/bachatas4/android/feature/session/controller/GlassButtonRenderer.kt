package com.bachatas4.android.feature.session.controller

import android.graphics.BlurMaskFilter
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import kotlin.math.min
import kotlin.math.roundToInt

object GlassButtonRenderer {

    var scale: Float = 1f

    // ── CSS variable equivalents (Boosted Opacity for High Visibility) ────
    private const val GLASS_BG            = 0xD9_0F_0F_14.toInt()  // rgba(15,15,20,0.85)
    private const val GLASS_BORDER        = 0xB3_FF_FF_FF.toInt()  // rgba(255,255,255,0.70)
    private const val GLASS_ACTIVE        = 0xB3_FF_FF_FF.toInt()  // rgba(255,255,255,0.70)
    private const val GLASS_LABEL         = 0xFF_FF_FF_FF.toInt()  // rgba(255,255,255,1.00)
    private const val GLASS_LABEL_DIM     = 0xB3_FF_FF_FF.toInt()  // rgba(255,255,255,0.70)
    private const val SHADOW_COLOR        = 0xB3_00_00_00.toInt()  // rgba(0,0,0,0.70)

    // Face-button colours (from HTML CSS vars) - Boosted Opacity
    private const val TRI_BG   = 0xF2_32_B4_82.toInt()  // rgba(50,180,130,0.95)
    private const val TRI_BOR  = 0xCC_50_D2_A0.toInt()  // rgba(80,210,160,0.80)
    private const val CRO_BG   = 0xF2_50_8C_DC.toInt()  // rgba(80,140,220,0.95)
    private const val CRO_BOR  = 0xCC_64_A5_F0.toInt()  // rgba(100,165,240,0.80)
    private const val SQ_BG    = 0xF2_C8_50_82.toInt()  // rgba(200,80,130,0.95)
    private const val SQ_BOR   = 0xCC_E1_6E_9B.toInt()  // rgba(225,110,155,0.80)
    private const val CIR_BG   = 0xF2_D2_46_3C.toInt()  // rgba(210,70,60,0.95)
    private const val CIR_BOR  = 0xCC_EB_64_5A.toInt()  // rgba(235,100,90,0.80)

    // PS button (Cyan theme to match PS4 controller) - Boosted Opacity
    private const val PS_BG    = 0xD9_14_20_23.toInt()
    private const val PS_BOR   = 0xBF_00_C6_FF.toInt()
    private const val PS_GLOW  = 0x80_00_C6_FF.toInt()
    private const val PS_LABEL = 0xFF_00_C6_FF.toInt()

    // ── Reusable Paint objects ─────────────────────────────────────────────
    private val fillPaint  = Paint(Paint.ANTI_ALIAS_FLAG)
    private val borderPaint= Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val textPaint  = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val glowPaint  = Paint(Paint.ANTI_ALIAS_FLAG)
    private val shineGradPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rectF      = RectF()

    private fun colorWithAlpha(baseColor: Int, extraAlpha: Int): Int {
        val a = ((Color.alpha(baseColor) * extraAlpha) / 255).coerceIn(0, 255)
        return Color.argb(a, Color.red(baseColor), Color.green(baseColor), Color.blue(baseColor))
    }

    fun drawFaceButton(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        label: String,
        control: String,
        overlayAlpha: Int = 255
    ) {
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val r  = (min(rect.width(), rect.height()) / 2f) - 2f * scale

        val (bgColor, borderColor) = when (control) {
            "triangle" -> Pair(TRI_BG, TRI_BOR)
            "circle" -> Pair(CIR_BG, CIR_BOR)
            "square" -> Pair(SQ_BG, SQ_BOR)
            else -> Pair(CRO_BG, CRO_BOR) // cross
        }

        // Drop shadow
        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(r * 0.4f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            canvas.drawCircle(cx, cy + r * 0.12f, r, glowPaint)
            glowPaint.maskFilter = null
        }

        // Color glow on pressed
        if (pressed) {
            glowPaint.maskFilter = BlurMaskFilter(r * 0.7f, BlurMaskFilter.Blur.NORMAL)
            val glowColor = Color.argb(
                (Color.alpha(borderColor) * overlayAlpha / 255),
                Color.red(borderColor), Color.green(borderColor), Color.blue(borderColor)
            )
            glowPaint.color = glowColor
            canvas.drawCircle(cx, cy, r + r * 0.15f, glowPaint)
            glowPaint.maskFilter = null
        }

        // Background fill
        val activeBg = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(bgColor, overlayAlpha)
        fillPaint.color = activeBg
        canvas.drawCircle(cx, cy, r, fillPaint)

        // Inset top shine
        if (!pressed) {
            shineGradPaint.shader = LinearGradient(
                cx, cy - r, cx, cy,
                intArrayOf(colorWithAlpha(0x1AFFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
                null, Shader.TileMode.CLAMP
            )
            canvas.drawCircle(cx, cy, r, shineGradPaint)
            shineGradPaint.shader = null
        }

        // Border - Black backdrop first for contrast
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.2f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Border - Colored main stroke
        val activeBorder = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(borderColor, overlayAlpha)
        borderPaint.color = activeBorder
        borderPaint.strokeWidth = 2.0f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Label with shadow
        val labelColor = colorWithAlpha(GLASS_LABEL, overlayAlpha)
        textPaint.color = labelColor
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        textPaint.textSize = r * 0.75f
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, cx, textY, textPaint)
        textPaint.clearShadowLayer()
    }

    fun drawShoulderButton(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        label: String,
        overlayAlpha: Int = 255
    ) {
        rectF.set(rect)
        val cornerR = rect.height() * 0.30f

        // Drop shadow
        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.5f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            rectF.set(rect.left.toFloat(), rect.top + rect.height() * 0.1f,
                rect.right.toFloat(), rect.bottom.toFloat() + rect.height() * 0.15f)
            canvas.drawRoundRect(rectF, cornerR, cornerR, glowPaint)
            glowPaint.maskFilter = null
            rectF.set(rect)
        }

        // Active white glow
        if (pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.6f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(0x38FFFFFF.toInt(), overlayAlpha)
            canvas.drawRoundRect(rectF, cornerR, cornerR, glowPaint)
            glowPaint.maskFilter = null
        }

        // Fill
        val bg = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BG, overlayAlpha)
        fillPaint.color = bg
        canvas.drawRoundRect(rectF, cornerR, cornerR, fillPaint)

        // Top shine
        if (!pressed) {
            shineGradPaint.shader = LinearGradient(
                0f, rect.top.toFloat(), 0f, rect.exactCenterY(),
                intArrayOf(colorWithAlpha(0x1AFFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
                null, Shader.TileMode.CLAMP
            )
            canvas.drawRoundRect(rectF, cornerR, cornerR, shineGradPaint)
            shineGradPaint.shader = null
        }

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.2f * scale
        val inset = 1f * scale
        rectF.set(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset)
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)

        // Border - White main stroke
        borderPaint.color = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BORDER, overlayAlpha)
        borderPaint.strokeWidth = 2.0f * scale
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)
        rectF.set(rect)

        // Label with shadow
        textPaint.color = colorWithAlpha(GLASS_LABEL, overlayAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        textPaint.textSize = rect.height() * 0.40f
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, cx, textY, textPaint)
        textPaint.clearShadowLayer()
    }

    fun drawDpadArm(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        label: String,
        topLeft: Boolean,
        topRight: Boolean,
        bottomLeft: Boolean,
        bottomRight: Boolean,
        overlayAlpha: Int = 255
    ) {
        val r  = rect.width() * 0.17f
        val path = buildSelectivePath(rect, r, topLeft, topRight, bottomLeft, bottomRight)

        // Drop shadow
        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.35f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            canvas.drawPath(path, glowPaint)
            glowPaint.maskFilter = null
        }

        // Fill
        val bg = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BG, overlayAlpha)
        fillPaint.color = bg
        canvas.drawPath(path, fillPaint)

        // Top shine
        if (!pressed) {
            shineGradPaint.shader = LinearGradient(
                0f, rect.top.toFloat(), 0f, rect.exactCenterY(),
                intArrayOf(colorWithAlpha(0x1AFFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
                null, Shader.TileMode.CLAMP
            )
            canvas.drawPath(path, shineGradPaint)
            shineGradPaint.shader = null
        }

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.2f * scale
        canvas.drawPath(path, borderPaint)

        // Border - White main stroke
        borderPaint.color = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BORDER, overlayAlpha)
        borderPaint.strokeWidth = 2.0f * scale
        canvas.drawPath(path, borderPaint)

        // Arrow glyph with shadow
        val arrowSize = min(rect.width(), rect.height()) * 0.38f
        textPaint.color = colorWithAlpha(GLASS_LABEL, overlayAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        textPaint.textSize = arrowSize
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, cx, textY, textPaint)
        textPaint.clearShadowLayer()
    }

    private fun buildSelectivePath(
        rect: Rect,
        r: Float,
        roundTopLeft: Boolean,
        roundTopRight: Boolean,
        roundBottomLeft: Boolean,
        roundBottomRight: Boolean
    ): android.graphics.Path {
        val path = android.graphics.Path()
        val l = rect.left.toFloat()
        val t = rect.top.toFloat()
        val right = rect.right.toFloat()
        val b = rect.bottom.toFloat()

        path.moveTo(l + if (roundTopLeft) r else 0f, t)
        path.lineTo(right - if (roundTopRight) r else 0f, t)
        if (roundTopRight)  path.quadTo(right, t, right, t + r)
        path.lineTo(right, b - if (roundBottomRight) r else 0f)
        if (roundBottomRight) path.quadTo(right, b, right - r, b)
        path.lineTo(l + if (roundBottomLeft) r else 0f, b)
        if (roundBottomLeft) path.quadTo(l, b, l, b - r)
        path.lineTo(l, t + if (roundTopLeft) r else 0f)
        if (roundTopLeft)   path.quadTo(l, t, l + r, t)
        path.close()
        return path
    }

    fun drawCenterButton(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        label: String,
        overlayAlpha: Int = 255
    ) {
        rectF.set(rect)
        val cornerR = rect.height() / 2f

        // shadow
        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.5f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            val shadowRect = RectF(rectF)
            shadowRect.offset(0f, rect.height() * 0.1f)
            canvas.drawRoundRect(shadowRect, cornerR, cornerR, glowPaint)
            glowPaint.maskFilter = null
        }

        if (pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.8f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(0x38FFFFFF.toInt(), overlayAlpha)
            canvas.drawRoundRect(rectF, cornerR, cornerR, glowPaint)
            glowPaint.maskFilter = null
        }

        // fill
        val bg = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BG, overlayAlpha)
        fillPaint.color = bg
        canvas.drawRoundRect(rectF, cornerR, cornerR, fillPaint)

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.2f * scale
        val inset = 1f * scale
        rectF.set(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset)
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)

        // Border - White main stroke
        borderPaint.color = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BORDER, overlayAlpha)
        borderPaint.strokeWidth = 2.0f * scale
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)

        // label with shadow
        textPaint.color = colorWithAlpha(GLASS_LABEL_DIM, overlayAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        textPaint.textSize = rect.height() * 0.38f
        textPaint.letterSpacing = 0.06f
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, cx, textY, textPaint)
        textPaint.clearShadowLayer()
        textPaint.letterSpacing = 0f
    }

    fun drawPsButton(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        overlayAlpha: Int = 255
    ) {
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val r  = min(rect.width(), rect.height()) / 2f - 2f * scale

        // Ambient cyan glow (always visible, stronger on press)
        val glowStrength = if (pressed) 0.8f else 0.4f
        glowPaint.maskFilter = BlurMaskFilter(r * 1.1f, BlurMaskFilter.Blur.NORMAL)
        val glowColor = colorWithAlpha(PS_GLOW, (overlayAlpha * glowStrength).toInt().coerceIn(0, 255))
        glowPaint.color = glowColor
        canvas.drawCircle(cx, cy, r * 1.1f, glowPaint)
        glowPaint.maskFilter = null

        // Drop shadow
        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(r * 0.5f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            canvas.drawCircle(cx, cy + r * 0.1f, r, glowPaint)
            glowPaint.maskFilter = null
        }

        // Active white ring on press
        if (pressed) {
            glowPaint.maskFilter = BlurMaskFilter(r * 0.6f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(0x38FFFFFF.toInt(), overlayAlpha)
            canvas.drawCircle(cx, cy, r + r * 0.15f, glowPaint)
            glowPaint.maskFilter = null
        }

        // Fill
        val bg = if (pressed)
            colorWithAlpha(0x66_00_C6_FF.toInt(), overlayAlpha)
        else
            colorWithAlpha(PS_BG, overlayAlpha)
        fillPaint.color = bg
        canvas.drawCircle(cx, cy, r, fillPaint)

        // Top shine
        if (!pressed) {
            shineGradPaint.shader = LinearGradient(
                cx, cy - r, cx, cy,
                intArrayOf(colorWithAlpha(0x1AFFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
                null, Shader.TileMode.CLAMP
            )
            canvas.drawCircle(cx, cy, r, shineGradPaint)
            shineGradPaint.shader = null
        }

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.8f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Border - Cyan main stroke
        borderPaint.color = colorWithAlpha(PS_BOR, overlayAlpha)
        borderPaint.strokeWidth = 2.5f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // "PS" label with shadow
        textPaint.color = colorWithAlpha(PS_LABEL, overlayAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        textPaint.textSize = r * 0.65f
        textPaint.letterSpacing = 0.02f
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText("PS", cx, textY, textPaint)
        textPaint.clearShadowLayer()
        textPaint.letterSpacing = 0f
    }

    fun drawStickRing(canvas: Canvas, rect: Rect, overlayAlpha: Int = 255) {
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val r  = min(rect.width(), rect.height()) / 2f - 2f * scale

        // Subtle outer glow
        glowPaint.maskFilter = BlurMaskFilter(r * 0.25f, BlurMaskFilter.Blur.NORMAL)
        glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
        canvas.drawCircle(cx, cy, r, glowPaint)
        glowPaint.maskFilter = null

        // Fill — rgba(128,128,128,0.40)
        fillPaint.color = colorWithAlpha(0x66_80_80_80.toInt(), overlayAlpha)
        canvas.drawCircle(cx, cy, r, fillPaint)

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 5.5f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Border — rgba(128,128,128,0.75)
        borderPaint.color = colorWithAlpha(0xBF_80_80_80.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 3f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)
    }

    fun drawStickNub(canvas: Canvas, rect: Rect, label: String, overlayAlpha: Int = 255) {
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val r  = min(rect.width(), rect.height()) / 2f - 2f * scale

        // Drop shadow
        glowPaint.maskFilter = BlurMaskFilter(r * 0.5f, BlurMaskFilter.Blur.NORMAL)
        glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
        canvas.drawCircle(cx, cy + r * 0.1f, r, glowPaint)
        glowPaint.maskFilter = null

        // Fill — glass-bg
        fillPaint.color = colorWithAlpha(GLASS_BG, overlayAlpha)
        canvas.drawCircle(cx, cy, r, fillPaint)

        // Top shine
        shineGradPaint.shader = LinearGradient(
            cx, cy - r, cx, cy,
            intArrayOf(colorWithAlpha(0x26FFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
            null, Shader.TileMode.CLAMP
        )
        canvas.drawCircle(cx, cy, r, shineGradPaint)
        shineGradPaint.shader = null

        // Border - Black backdrop
        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.8f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Border — rgba(255,255,255,0.22)
        borderPaint.color = colorWithAlpha(0x38_FF_FF_FF.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 2.5f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        // Label (L3/R3) with shadow
        textPaint.color = colorWithAlpha(GLASS_LABEL_DIM, overlayAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
        textPaint.textSize = r * 0.50f
        textPaint.letterSpacing = 0.04f
        textPaint.setShadowLayer(4f * scale, 0f, 1f * scale, 0xFF_00_00_00.toInt())
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText(label, cx, textY, textPaint)
        textPaint.clearShadowLayer()
        textPaint.letterSpacing = 0f
    }

    fun drawTouchpad(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        overlayAlpha: Int = 255
    ) {
        val touchpadAlpha = (overlayAlpha * 0.15f).roundToInt()
        rectF.set(rect)
        val cornerR = rect.height() / 2f // fully rounded capsule

        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(rect.height() * 0.3f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, touchpadAlpha)
            canvas.drawRoundRect(rectF, cornerR, cornerR, glowPaint)
            glowPaint.maskFilter = null
        }

        val bg = if (pressed) colorWithAlpha(GLASS_ACTIVE, touchpadAlpha) else colorWithAlpha(GLASS_BG, touchpadAlpha)
        fillPaint.color = bg
        canvas.drawRoundRect(rectF, cornerR, cornerR, fillPaint)

        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), touchpadAlpha)
        borderPaint.strokeWidth = 3.5f * scale
        val inset = 1f * scale
        rectF.set(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset)
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)

        borderPaint.color = if (pressed) colorWithAlpha(GLASS_ACTIVE, touchpadAlpha) else colorWithAlpha(GLASS_BORDER, touchpadAlpha)
        borderPaint.strokeWidth = 1.5f * scale
        canvas.drawRoundRect(rectF, cornerR - inset, cornerR - inset, borderPaint)
        rectF.set(rect)

        textPaint.color = colorWithAlpha(GLASS_LABEL_DIM, touchpadAlpha)
        textPaint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
        textPaint.textSize = 10f * scale
        textPaint.setShadowLayer(2f * scale, 0f, 0.5f * scale, 0xFF_00_00_00.toInt())
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val textY = cy - (textPaint.descent() + textPaint.ascent()) / 2f
        canvas.drawText("TOUCHPAD", cx, textY, textPaint)
        textPaint.clearShadowLayer()
    }

    fun drawIconCenterButton(
        canvas: Canvas,
        rect: Rect,
        pressed: Boolean,
        isShare: Boolean,
        overlayAlpha: Int = 255
    ) {
        val cx = rect.exactCenterX()
        val cy = rect.exactCenterY()
        val r  = min(rect.width(), rect.height()) / 2f - 2f * scale

        if (!pressed) {
            glowPaint.maskFilter = BlurMaskFilter(r * 0.5f, BlurMaskFilter.Blur.NORMAL)
            glowPaint.color = colorWithAlpha(SHADOW_COLOR, overlayAlpha)
            canvas.drawCircle(cx, cy + r * 0.1f, r, glowPaint)
            glowPaint.maskFilter = null
        }

        val bg = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BG, overlayAlpha)
        fillPaint.color = bg
        canvas.drawCircle(cx, cy, r, fillPaint)

        if (!pressed) {
            shineGradPaint.shader = LinearGradient(
                cx, cy - r, cx, cy,
                intArrayOf(colorWithAlpha(0x1AFFFFFF.toInt(), overlayAlpha), Color.TRANSPARENT),
                null, Shader.TileMode.CLAMP
            )
            canvas.drawCircle(cx, cy, r, shineGradPaint)
            shineGradPaint.shader = null
        }

        borderPaint.color = colorWithAlpha(0xD8_00_00_00.toInt(), overlayAlpha)
        borderPaint.strokeWidth = 4.2f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        borderPaint.color = if (pressed) colorWithAlpha(GLASS_ACTIVE, overlayAlpha) else colorWithAlpha(GLASS_BORDER, overlayAlpha)
        borderPaint.strokeWidth = 1.8f * scale
        canvas.drawCircle(cx, cy, r - 1f * scale, borderPaint)

        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = colorWithAlpha(GLASS_LABEL, overlayAlpha)
            style = Paint.Style.STROKE
            strokeWidth = 2.5f * scale
            strokeCap = Paint.Cap.ROUND
        }

        if (isShare) {
            val size = r * 0.8f
            val x1 = cx - size * 0.25f
            val y1 = cy

            val x2 = cx + size * 0.25f
            val y2 = cy - size * 0.25f

            val x3 = cx + size * 0.25f
            val y3 = cy + size * 0.25f

            canvas.drawLine(x1, y1, x2, y2, paint)
            canvas.drawLine(x1, y1, x3, y3, paint)

            paint.style = Paint.Style.FILL
            val nodeR = size * 0.12f
            canvas.drawCircle(x1, y1, nodeR, paint)
            canvas.drawCircle(x2, y2, nodeR, paint)
            canvas.drawCircle(x3, y3, nodeR, paint)
        } else {
            val lineW = r * 0.7f
            val gap = r * 0.22f
            canvas.drawLine(cx - lineW/2, cy - gap, cx + lineW/2, cy - gap, paint)
            canvas.drawLine(cx - lineW/2, cy, cx + lineW/2, cy, paint)
            canvas.drawLine(cx - lineW/2, cy + gap, cx + lineW/2, cy + gap, paint)
        }
    }
}
