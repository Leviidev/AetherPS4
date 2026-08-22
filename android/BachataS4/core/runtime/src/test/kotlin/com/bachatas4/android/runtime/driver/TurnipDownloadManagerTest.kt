package com.bachatas4.android.runtime.driver

import java.io.ByteArrayInputStream
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TurnipDownloadManagerTest {
    @Test
    fun downloadsWithProgressAndInstallsTrustedSourceMetadata() {
        val archive = turnipZip(DriverAbi.ANDROID_BIONIC).readBytes()
        val asset = asset(archive.size.toLong())
        val progress = mutableListOf<Long>()
        val manager = TurnipDownloadManager(
            TurnipPackageInstaller(Files.createTempDirectory("download-install"), 37),
            DriverAssetTransport { DownloadResponse(200, archive.size.toLong(), ByteArrayInputStream(archive)) },
        )

        val installed = manager.download(asset) { copied, _ -> progress += copied }

        assertEquals(TurnipReleaseClient.REPOSITORY, installed.metadata.sourceRepository)
        assertEquals(asset.releaseTag, installed.metadata.releaseTag)
        assertEquals(archive.size.toLong(), progress.last())
    }

    @Test
    fun rejectsLengthMismatchBeforeInstalling() {
        val root = Files.createTempDirectory("download-install")
        val archive = turnipZip(DriverAbi.ANDROID_BIONIC).readBytes()
        val manager = TurnipDownloadManager(
            TurnipPackageInstaller(root, 37),
            DriverAssetTransport { DownloadResponse(200, archive.size.toLong(), ByteArrayInputStream(archive)) },
        )

        assertThrows(IllegalArgumentException::class.java) {
            manager.download(asset(archive.size.toLong() + 1)) { _, _ -> }
        }
        assertTrue(DriverRegistry(root).listInstalled().isEmpty())
    }

    private fun asset(size: Long) = TurnipReleaseAsset(
        releaseTag = "31_may_2026",
        publishedAt = "2026-05-30T21:07:14Z",
        name = "Turnip-26-1.1-EMULATOR.zip",
        size = size,
        downloadUrl = "https://github.com/JICA98/bachata-s4-drivers/releases/download/tag/Turnip.zip",
    )
}
