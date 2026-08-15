package com.bachatas4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class InstallManifestTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun writeAndReadRoundTrip() {
        val dir = temporaryFolder.newFolder("game")
        val m = InstallManifest(
            status = InstallManifestIo.STATUS_INSTALLED,
            gameId = "CUSA00000",
            contentId = "EP0001-CUSA00000_00-TEST",
            mode = "pkg",
            sourceUri = "content://pkg",
            installedAtMs = 123L,
            requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
            bytesTotal = 99L,
        )
        InstallManifestIo.write(dir, m)
        val read = InstallManifestIo.read(dir)
        assertNotNull(read)
        assertEquals("CUSA00000", read!!.gameId)
        assertEquals(InstallManifestIo.STATUS_INSTALLED, read.status)
        assertEquals(99L, read.bytesTotal)
        assertEquals("pkg", read.mode)
    }

    @Test
    fun overlayRoundTripsWithManifest() {
        val dir = temporaryFolder.newFolder("overlay")
        val manifest = InstallManifest(
            version = 2,
            status = InstallManifestIo.STATUS_INSTALLED,
            gameId = "CUSA07023",
            contentId = "EP9000-CUSA07023_00-BBBASEGAME00000",
            mode = "pkg",
            sourceUri = "content://base",
            installedAtMs = 1_700_000_000_000L,
            requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
            bytesTotal = 10_000L,
            overlays = listOf(
                OverlayRecord("EP9000-CUSA07023_00-BBUPDT010000000", "content://u1", 1_700_000_001_000L),
                OverlayRecord(null, "content://dlc", 1_700_000_002_000L),
            ),
        )
        InstallManifestIo.write(dir, manifest)
        val read = InstallManifestIo.read(dir)
        assertEquals(manifest, read)
    }

    @Test
    fun versionOneManifestReadsWithEmptyOverlays() {
        val dir = temporaryFolder.newFolder("v1")
        // Hand-write a v1 manifest with no overlays line.
        File(dir, InstallManifestIo.FILE_NAME).writeText(
            """
            version=1
            status=INSTALLED
            gameId=CUSA07023
            contentId=
            mode=pkg
            sourceUri=content://base
            installedAtMs=1700000000000
            requiredFiles=eboot.bin,sce_sys/param.sfo
            bytesTotal=10000
            """.trimIndent(),
        )
        val read = InstallManifestIo.read(dir)
        assertNotNull(read)
        assertEquals(1, read!!.version)
        assertEquals(emptyList<OverlayRecord>(), read.overlays)
    }
}
