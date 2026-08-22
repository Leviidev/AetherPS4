package com.bachatas4.android.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

data class ParamSfoMetadata(
    val title: String?,
    val titleId: String?,
    val subtitle: String? = null,
    val detail: String? = null,
)

/**
 * Minimal PS4/PSF param.sfo reader for library metadata.
 * Matches desktop layout in `src/core/file_format/psf.*` for string keys.
 */
object ParamSfoReader {
    private const val MAGIC = 0x00505346
    private const val VERSION_1_0 = 0x00000100
    private const val VERSION_1_1 = 0x00000101
    private const val FMT_TEXT = 0x0204
    private const val HEADER_SIZE = 20
    private const val INDEX_ENTRY_SIZE = 16

    fun parse(bytes: ByteArray): ParamSfoMetadata {
        if (bytes.size < HEADER_SIZE) return ParamSfoMetadata(null, null)
        return try {
            parseOrThrow(bytes)
        } catch (_: Exception) {
            ParamSfoMetadata(null, null)
        }
    }

    private fun parseOrThrow(bytes: ByteArray): ParamSfoMetadata {
        val buf = ByteBuffer.wrap(bytes)
        buf.order(ByteOrder.BIG_ENDIAN)
        val magic = buf.int
        if (magic != MAGIC) return ParamSfoMetadata(null, null)
        buf.order(ByteOrder.LITTLE_ENDIAN)
        val version = buf.int
        if (version != VERSION_1_0 && version != VERSION_1_1) return ParamSfoMetadata(null, null)
        val keyTableOffset = buf.int
        val dataTableOffset = buf.int
        val count = buf.int
        if (count < 0 || count > 4096) return ParamSfoMetadata(null, null)

        var title: String? = null
        var titleId: String? = null
        var subtitle: String? = null
        var detail: String? = null
        for (i in 0 until count) {
            val entryPos = HEADER_SIZE + i * INDEX_ENTRY_SIZE
            if (entryPos + INDEX_ENTRY_SIZE > bytes.size) return ParamSfoMetadata(null, null)
            buf.position(entryPos)
            buf.order(ByteOrder.LITTLE_ENDIAN)
            val keyOffset = buf.short.toInt() and 0xFFFF
            val fmt = buf.short.toInt() and 0xFFFF
            val len = buf.int
            buf.int // maxLen
            val dataOffset = buf.int
            if (fmt != FMT_TEXT) continue
            val key = readCString(bytes, keyTableOffset + keyOffset) ?: continue
            val value = readCString(bytes, dataTableOffset + dataOffset, maxLen = len) ?: continue
            when (key) {
                "TITLE" -> title = value.ifBlank { null }
                "TITLE_ID" -> titleId = value.ifBlank { null }
                "SUBTITLE" -> subtitle = value.ifBlank { null }
                "DETAIL" -> detail = value.ifBlank { null }
            }
        }
        return ParamSfoMetadata(title = title, titleId = titleId, subtitle = subtitle, detail = detail)
    }

    private fun readCString(bytes: ByteArray, start: Int, maxLen: Int = Int.MAX_VALUE): String? {
        if (start < 0 || start >= bytes.size) return null
        val endLimit = minOf(bytes.size, if (maxLen == Int.MAX_VALUE) bytes.size else start + maxLen.coerceAtLeast(0))
        var end = start
        while (end < endLimit && bytes[end] != 0.toByte()) end++
        if (end == start) return ""
        return String(bytes, start, end - start, Charsets.UTF_8)
    }
}
