package com.bachatas4.android.runtime.driver

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

internal fun turnipZip(
    abi: DriverAbi,
    extraEntries: List<Pair<String, ByteArray>> = emptyList(),
): ByteArrayInputStream {
    val libraryName = "vulkan.turnip.so"
    val metadata = """
        {
          "schemaVersion": 1,
          "name": "Freedreno Turnip Driver 26.1.1",
          "packageVersion": "3",
          "minApi": 34,
          "libraryName": "$libraryName"
        }
    """.trimIndent().toByteArray()
    val output = ByteArrayOutputStream()
    ZipOutputStream(output).use { zip ->
        fun entry(name: String, bytes: ByteArray) {
            zip.putNextEntry(ZipEntry(name).apply { time = 0L })
            zip.write(bytes)
            zip.closeEntry()
        }
        entry("meta.json", metadata)
        entry(libraryName, arm64Elf(abi))
        extraEntries.forEach { (name, bytes) -> entry(name, bytes) }
    }
    return ByteArrayInputStream(output.toByteArray())
}

private fun arm64Elf(abi: DriverAbi): ByteArray = ByteArray(160).also { bytes ->
    bytes[0] = 0x7f
    bytes[1] = 'E'.code.toByte()
    bytes[2] = 'L'.code.toByte()
    bytes[3] = 'F'.code.toByte()
    bytes[4] = 2
    bytes[5] = 1
    bytes[18] = 183.toByte()
    bytes[19] = 0
    val marker = if (abi == DriverAbi.LINUX_GLIBC) "libc.so.6" else "libc.so"
    marker.toByteArray().copyInto(bytes, 64)
}
