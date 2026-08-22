package com.bachatas4.android.feature.session.controller

data class NormalizedInsets(val left: Float = 0f, val top: Float = 0f, val right: Float = 0f, val bottom: Float = 0f)

object TouchLayoutRenderer {
    fun hitTest(layout: TouchLayout, x: Float, y: Float): TouchControlPlacement? = layout.controls
        .asSequence().filter { it.visible && contains(layout, it, x, y) }.maxByOrNull { it.zIndex }

    fun move(layout: TouchLayout, control: String, x: Float, y: Float, insets: NormalizedInsets = NormalizedInsets()): TouchLayout =
        edit(layout, control) { placement ->
            val halfW = placement.width * layout.scale / 2f
            val halfH = placement.height * layout.scale / 2f
            placement.copy(
                centerX = x.coerceIn(insets.left + halfW, 1f - insets.right - halfW),
                centerY = y.coerceIn(insets.top + halfH, 1f - insets.bottom - halfH),
            )
        }

    fun resize(layout: TouchLayout, control: String, width: Float, height: Float): TouchLayout = edit(layout, control) {
        it.copy(width = width.coerceIn(TouchControlPlacement.MIN_SIZE, 1f), height = height.coerceIn(TouchControlPlacement.MIN_SIZE, 1f))
    }

    fun setVisible(layout: TouchLayout, control: String, visible: Boolean): TouchLayout = edit(layout, control) { it.copy(visible = visible) }
    fun bringToFront(layout: TouchLayout, control: String): TouchLayout = edit(layout, control) { it.copy(zIndex = (layout.controls.maxOfOrNull(TouchControlPlacement::zIndex) ?: 0) + 1) }

    private fun edit(layout: TouchLayout, control: String, block: (TouchControlPlacement) -> TouchControlPlacement): TouchLayout {
        require(layout.controls.any { it.control == control }) { "Unknown touch control $control" }
        return layout.copy(controls = layout.controls.map { if (it.control == control) block(it) else it })
    }

    private fun contains(layout: TouchLayout, p: TouchControlPlacement, x: Float, y: Float): Boolean {
        val halfW = p.width * layout.scale / 2f
        val halfH = p.height * layout.scale / 2f
        return x in p.centerX - halfW..p.centerX + halfW && y in p.centerY - halfH..p.centerY + halfH
    }
}
