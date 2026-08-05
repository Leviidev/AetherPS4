package com.bachatas4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class LibrarySyncTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun incompleteFolderNotScheduledForInsert() {
        val root = temporaryFolder.root
        val games = File(root, "games").apply { mkdirs() }
        File(games, "HALF").mkdirs()
        val plan = LibrarySync.planSync(games, dbIds = emptySet(), importBusy = false, filesDir = root)
        assertTrue(plan.inserts.isEmpty())
    }

    @Test
    fun completeTreeWithoutManifestHealsAndInserts() {
        val root = temporaryFolder.root
        val games = File(root, "games").apply { mkdirs() }
        val g = File(games, "CUSA00000").apply { mkdirs() }
        File(g, "sce_sys").mkdirs()
        File(g, "sce_sys/param.sfo").writeBytes(buildMinimalSfo(mapOf("TITLE" to "T", "TITLE_ID" to "CUSA00000")))
        File(g, "eboot.bin").writeBytes(byteArrayOf(1))
        val plan = LibrarySync.planSync(games, emptySet(), false, filesDir = root)
        assertEquals(1, plan.inserts.size)
        assertEquals("CUSA00000", plan.inserts[0].id)
        assertTrue(plan.healManifestDirs.isNotEmpty())
        LibrarySync.applyHeals(plan)
        assertTrue(InstallManifestIo.read(g) != null)
    }

    @Test
    fun orphanImportDirScheduledCleanupWhenIdle() {
        val root = temporaryFolder.root
        val games = File(root, "games").apply { mkdirs() }
        File(games, ".import-x").mkdirs()
        val plan = LibrarySync.planSync(games, emptySet(), importBusy = false, filesDir = root)
        assertTrue(plan.stagingDirsToDelete.any { it.name == ".import-x" })
    }
}
