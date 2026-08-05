package com.bachatas4.android.data

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
}
