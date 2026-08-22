package com.bachatas4.android.runtime.process

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class CustomVulkanDriverInstallerTest {
    @Test
    fun installsArm64GlibcDriverAndGeneratesAbsoluteIcd() {
        val root = Files.createTempDirectory("custom-turnip")
        val installer = CustomVulkanDriverInstaller(root)

        val installed = installer.install(packageZip(arm64Elf(glibc = true)))

        assertEquals("Test Turnip", installed.name)
        assertTrue(Files.isRegularFile(installed.library))
        assertTrue(Files.readString(installed.icdManifest).contains(installed.library.toAbsolutePath().toString()))
        assertEquals(installed, installer.load())
    }

    @Test
    fun rejectsAndroidBionicDriver() {
        val installer = CustomVulkanDriverInstaller(Files.createTempDirectory("custom-turnip"))

        expectFailure("glibc") { installer.install(packageZip(arm64Elf(glibc = false))) }
    }

    @Test
    fun rejectsArchivePathTraversal() {
        val installer = CustomVulkanDriverInstaller(Files.createTempDirectory("custom-turnip"))

        expectFailure("entry") {
            installer.install(packageZip(arm64Elf(glibc = true), extraName = "../escape"))
        }
    }

    private fun packageZip(library: ByteArray, extraName: String? = null): ByteArrayInputStream {
        val output = ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            fun entry(name: String, bytes: ByteArray) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(bytes)
                zip.closeEntry()
            }
            entry("meta.json", """{"schemaVersion":1,"name":"Test Turnip","libraryName":"libvulkan_freedreno.so","abi":"linux-aarch64-glibc"}""".toByteArray())
            entry("libvulkan_freedreno.so", library)
            extraName?.let { entry(it, byteArrayOf(1)) }
        }
        return ByteArrayInputStream(output.toByteArray())
    }

    private fun arm64Elf(glibc: Boolean): ByteArray = ByteArray(128).also { bytes ->
        bytes[0] = 0x7f
        bytes[1] = 'E'.code.toByte()
        bytes[2] = 'L'.code.toByte()
        bytes[3] = 'F'.code.toByte()
        bytes[4] = 2
        bytes[5] = 1
        bytes[18] = 183.toByte()
        bytes[19] = 0
        val marker = if (glibc) "libc.so.6" else "libc.so"
        marker.toByteArray().copyInto(bytes, 64)
    }

    private fun expectFailure(message: String, block: () -> Unit) {
        try {
            block()
            fail("Expected import failure")
        } catch (error: IllegalArgumentException) {
            assertTrue(error.message.orEmpty().contains(message, ignoreCase = true))
        }
    }
}
