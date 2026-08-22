package com.bachatas4.android.runtime.driver

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TurnipPackageInstallerTest {
    @Test
    fun installsTrustedEmulatorLayoutAsImmutableBionicDriver() {
        val root = Files.createTempDirectory("turnip-registry")
        val installer = TurnipPackageInstaller(root, deviceApi = 37)

        val installed = installer.install(
            turnipZip(DriverAbi.ANDROID_BIONIC),
            DriverPackageSource(
                repository = "JICA98/bachata-s4-drivers",
                releaseTag = "31_may_2026",
                assetName = "Turnip-26-1.1-EMULATOR.zip",
            ),
        )

        assertTrue(installed.metadata.id.matches(Regex("turnip-[0-9a-f]{16}")))
        assertEquals(DriverAbi.ANDROID_BIONIC, installed.metadata.abi)
        assertEquals("31_may_2026", installed.metadata.releaseTag)
        assertTrue(Files.isRegularFile(installed.library))
        assertEquals(null, installed.icdManifest)
        assertEquals(installed, installer.install(turnipZip(DriverAbi.ANDROID_BIONIC), installed.metadata.source()))
    }

    @Test
    fun installsGlibcDriverAndWritesPrivateIcd() {
        val root = Files.createTempDirectory("turnip-glibc")
        val installed = TurnipPackageInstaller(root, deviceApi = 37).install(
            turnipZip(DriverAbi.LINUX_GLIBC),
            DriverPackageSource(assetName = "import.zip"),
        )

        assertEquals(DriverAbi.LINUX_GLIBC, installed.metadata.abi)
        assertTrue(Files.isRegularFile(installed.icdManifest))
        assertTrue(Files.readString(installed.icdManifest).contains(installed.library.toString()))
    }

    @Test
    fun rejectsTraversalNestedArchivesAndIncompatibleApi() {
        val root = Files.createTempDirectory("turnip-invalid")
        val installer = TurnipPackageInstaller(root, deviceApi = 33)

        assertThrows(IllegalArgumentException::class.java) {
            installer.install(
                turnipZip(DriverAbi.ANDROID_BIONIC, listOf("../escape" to byteArrayOf(1))),
                DriverPackageSource(assetName = "bad.zip"),
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            installer.install(
                turnipZip(DriverAbi.ANDROID_BIONIC, listOf("nested.zip" to byteArrayOf(1))),
                DriverPackageSource(assetName = "bad.zip"),
            )
        }
        assertFalse(Files.list(root).use { it.findAny().isPresent })
    }
}
