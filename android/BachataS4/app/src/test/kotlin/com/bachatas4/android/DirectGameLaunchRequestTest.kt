package com.bachatas4.android

import com.bachatas4.android.data.InstallManifest
import com.bachatas4.android.data.InstallManifestIo
import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DirectGameLaunchRequestTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun resolvesInstalledTitleId() {
        val filesDir = temporaryFolder.newFolder("files")
        val game = File(filesDir, "games/CUSA07023").apply { mkdirs() }
        File(game, "eboot.bin").writeBytes(byteArrayOf(1))
        InstallManifestIo.write(
            game,
            InstallManifest(
                status = InstallManifestIo.STATUS_INSTALLED,
                gameId = "CUSA07023",
                contentId = null,
                mode = "pkg",
                sourceUri = "",
                installedAtMs = 1L,
                requiredFiles = listOf("eboot.bin"),
                bytesTotal = 1L,
            ),
        )

        assertEquals(
            DirectGameLaunchRequest.Resolution.Ready("CUSA07023"),
            DirectGameLaunchRequest.resolve(filesDir, "CUSA07023"),
        )
    }

    @Test
    fun resolveRejectsWhenInstallManifestMissing() {
        val filesDir = temporaryFolder.newFolder("files")
        val game = File(filesDir, "games/CUSA00000").apply { mkdirs() }
        File(game, "eboot.bin").writeBytes(byteArrayOf(1))
        assertTrue(
            DirectGameLaunchRequest.resolve(filesDir, "CUSA00000") is
                DirectGameLaunchRequest.Resolution.Rejected,
        )
    }

    @Test
    fun rejectsMissingMalformedAndUninstalledIds() {
        val filesDir = temporaryFolder.newFolder("files")
        val rejected = listOf(null, "", "cusa07023", "CUSA0702", "../CUSA07023", "CUSA99999")

        rejected.forEach { gameId ->
            assertTrue(
                "$gameId must be rejected",
                DirectGameLaunchRequest.resolve(filesDir, gameId) is
                    DirectGameLaunchRequest.Resolution.Rejected,
            )
        }
    }

    @Test
    fun rejectsGameDirectorySymlinkEscapingAppStorage() {
        val filesDir = temporaryFolder.newFolder("files")
        val gamesDir = File(filesDir, "games").apply { mkdirs() }
        val outside = temporaryFolder.newFolder("outside")
        File(outside, "eboot.bin").createNewFile()
        Files.createSymbolicLink(File(gamesDir, "CUSA07023").toPath(), outside.toPath())

        assertTrue(
            DirectGameLaunchRequest.resolve(filesDir, "CUSA07023") is
                DirectGameLaunchRequest.Resolution.Rejected,
        )
    }
}
