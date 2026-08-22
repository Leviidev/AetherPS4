package com.bachatas4.android.runtime.driver

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DriverRegistryTest {
    @Test
    fun listsResolvesAndRemovesInstalledVersions() {
        val root = Files.createTempDirectory("driver-registry")
        val installed = TurnipPackageInstaller(root, deviceApi = 37).install(
            turnipZip(DriverAbi.ANDROID_BIONIC),
            DriverPackageSource(assetName = "Turnip-26-1.1-EMULATOR.zip"),
        )
        val registry = DriverRegistry(root)

        assertEquals(listOf(installed), registry.listInstalled())
        assertEquals(installed, registry.resolve(installed.metadata.id))
        assertTrue(registry.remove(installed.metadata.id))
        assertNull(registry.resolve(installed.metadata.id))
    }
}
