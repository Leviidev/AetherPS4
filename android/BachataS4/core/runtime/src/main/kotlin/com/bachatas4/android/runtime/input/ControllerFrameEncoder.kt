package com.bachatas4.android.runtime.input

import kotlin.math.roundToInt

class ControllerFrameEncoder {
    private val sequence = LongArray(4)
    private val previous = arrayOfNulls<ControllerSnapshot>(4)

    @Synchronized
    fun encode(snapshot: ControllerSnapshot): ByteArray? = encode(0, snapshot)

    @Synchronized
    fun encode(slot: Int, snapshot: ControllerSnapshot): ByteArray? {
        require(slot in 0..3) { "Controller slot must be 0..3" }
        if (snapshot == previous[slot]) return null
        previous[slot] = snapshot
        sequence[slot] += 1
        val frame = buildString(144) {
            append("BACHATA/1 INPUT slot=").append(slot)
            append(" seq=").append(sequence[slot])
            append(" buttons=").append(snapshot.buttons)
            append(" lx=").append(stickByte(snapshot.leftX))
            append(" ly=").append(stickByte(snapshot.leftY))
            append(" rx=").append(stickByte(snapshot.rightX))
            append(" ry=").append(stickByte(snapshot.rightY))
            append(" l2=").append(triggerByte(snapshot.leftTrigger))
            append(" r2=").append(triggerByte(snapshot.rightTrigger))
            append(" touch=").append(if (snapshot.touchDown) 1 else 0)
            append(" tx=").append((snapshot.touchX * 1919f).roundToInt())
            append(" ty=").append((snapshot.touchY * 1079f).roundToInt())
            append('\n')
        }
        return frame.encodeToByteArray()
    }

    private fun stickByte(value: Float): Int = ((value + 1f) * 127.5f).roundToInt().coerceIn(0, 255)
    private fun triggerByte(value: Float): Int = (value * 255f).roundToInt().coerceIn(0, 255)
}
