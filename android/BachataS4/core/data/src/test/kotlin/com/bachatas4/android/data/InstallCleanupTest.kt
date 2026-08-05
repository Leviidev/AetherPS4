package com.bachatas4.android.data

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class InstallCleanupTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun cleanupJobRemovesStagingCacheAndJobDir() {
        val filesDir = temporaryFolder.root
        val store = InstallJobStore(filesDir)
        val jobId = "j1"
        val staging = File(filesDir, "games/.import-$jobId").apply { mkdirs() }
        File(staging, "x").writeText("x")
        val cache = File(filesDir, "pkg-cache/$jobId.pkg").apply {
            parentFile!!.mkdirs()
            writeText("pkg")
        }
        store.create(
            InstallJob(
                jobId = jobId,
                state = InstallJob.STATE_EXTRACTING,
                mode = ImportManager.MODE_PKG,
                sourceUri = "content://x",
                stagingDir = "games/.import-$jobId",
                cachePath = "pkg-cache/$jobId.pkg",
                createdAtMs = 1L,
                updatedAtMs = 1L,
            ),
        )
        InstallCleanup(filesDir, store).cleanupJob(jobId)
        assertFalse(staging.exists())
        assertFalse(cache.exists())
        assertNull(store.read(jobId))
    }

    @Test
    fun cleanupStaleArtifactsRemovesImportDirsWhenIdle() {
        val filesDir = temporaryFolder.root
        val orphan = File(filesDir, "games/.import-orphan").apply { mkdirs() }
        InstallCleanup(filesDir, InstallJobStore(filesDir)).cleanupStaleArtifacts(importBusy = false)
        assertFalse(orphan.exists())
    }

    @Test
    fun cleanupStaleArtifactsSkipsImportDirsWhenBusy() {
        val filesDir = temporaryFolder.root
        val orphan = File(filesDir, "games/.import-active").apply { mkdirs() }
        InstallCleanup(filesDir, InstallJobStore(filesDir)).cleanupStaleArtifacts(importBusy = true)
        assertTrue(orphan.exists())
    }
}
