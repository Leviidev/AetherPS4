package com.bachatas4.android.data

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ParamSfoReaderTest {
    @Test
    fun parsesTitleAndTitleId() {
        val bytes = buildMinimalSfo(
            mapOf(
                "TITLE" to "Bloodborne",
                "TITLE_ID" to "CUSA00900",
            ),
        )
        val meta = ParamSfoReader.parse(bytes)
        assertEquals("Bloodborne", meta.title)
        assertEquals("CUSA00900", meta.titleId)
    }

    @Test
    fun missingKeysReturnNullFields() {
        val bytes = buildMinimalSfo(mapOf("CATEGORY" to "gd"))
        val meta = ParamSfoReader.parse(bytes)
        assertNull(meta.title)
        assertNull(meta.titleId)
    }

    @Test
    fun truncatedOrInvalidReturnsEmptyMetadata() {
        assertEquals(ParamSfoMetadata(null, null), ParamSfoReader.parse(ByteArray(0)))
        assertEquals(ParamSfoMetadata(null, null), ParamSfoReader.parse("not-sfo".toByteArray()))
        assertEquals(ParamSfoMetadata(null, null), ParamSfoReader.parse(ByteArray(8) { 0 }))
    }
}

/** Minimal UTF-8 text-only SFO sufficient for unit tests. */
internal fun buildMinimalSfo(strings: Map<String, String>): ByteArray {
    val entries = strings.entries.toList()
    val headerSize = 20
    val indexSize = entries.size * 16
    val keyTable = ArrayList<Byte>()
    val keyOffsets = IntArray(entries.size)
    entries.forEachIndexed { i, (key, _) ->
        keyOffsets[i] = keyTable.size
        keyTable.addAll(key.toByteArray(Charsets.UTF_8).toList())
        keyTable.add(0)
    }
    while (keyTable.size % 4 != 0) keyTable.add(0)

    val dataTable = ArrayList<Byte>()
    val dataOffsets = IntArray(entries.size)
    val lengths = IntArray(entries.size)
    entries.forEachIndexed { i, (_, value) ->
        dataOffsets[i] = dataTable.size
        val raw = value.toByteArray(Charsets.UTF_8) + byteArrayOf(0)
        lengths[i] = raw.size
        dataTable.addAll(raw.toList())
        while (dataTable.size % 4 != 0) dataTable.add(0)
    }

    val keyTableOffset = headerSize + indexSize
    val dataTableOffset = keyTableOffset + keyTable.size
    val out = ByteBuffer.allocate(dataTableOffset + dataTable.size).order(ByteOrder.LITTLE_ENDIAN)
    // magic big-endian 0x00505346
    out.put(0, 0x00.toByte())
    out.put(1, 0x50.toByte())
    out.put(2, 0x53.toByte())
    out.put(3, 0x46.toByte())
    out.position(4)
    out.putInt(0x00000101) // version LE
    out.putInt(keyTableOffset)
    out.putInt(dataTableOffset)
    out.putInt(entries.size)
    entries.forEachIndexed { i, _ ->
        out.putShort(keyOffsets[i].toShort())
        // param_fmt as big-endian u16 0x0204 (Text) stored as bytes 04 02 in memory
        out.put(0x04.toByte())
        out.put(0x02.toByte())
        out.putInt(lengths[i])
        out.putInt(lengths[i])
        out.putInt(dataOffsets[i])
    }
    keyTable.forEach { out.put(it) }
    dataTable.forEach { out.put(it) }
    return out.array()
}
