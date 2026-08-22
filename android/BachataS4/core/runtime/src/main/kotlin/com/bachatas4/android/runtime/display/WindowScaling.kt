package com.bachatas4.android.runtime.display

data class FloatBounds(
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
)

fun aspectFitBounds(sourceWidth: Int, sourceHeight: Int, targetWidth: Int, targetHeight: Int): FloatBounds {
    require(sourceWidth > 0 && sourceHeight > 0 && targetWidth > 0 && targetHeight > 0)
    val scale = minOf(targetWidth.toFloat() / sourceWidth, targetHeight.toFloat() / sourceHeight)
    val width = sourceWidth * scale
    val height = sourceHeight * scale
    val left = (targetWidth - width) * 0.5f
    val top = (targetHeight - height) * 0.5f
    return FloatBounds(left, top, left + width, top + height)
}
