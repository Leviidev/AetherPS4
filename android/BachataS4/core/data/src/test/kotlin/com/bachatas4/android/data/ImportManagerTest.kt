package com.bachatas4.android.data

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ImportManagerTest {
    @After
    fun tearDown() {
        ImportManager.reset()
    }

    @Test
    fun tryBeginImportClaimsSlotWithSelected() {
        assertTrue(ImportManager.tryBeginImport("content://x", ImportManager.MODE_PKG))
        assertTrue(ImportManager.progress.value is ImportProgress.Selected)
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
    }

    @Test
    fun validatingThroughRegisteringAreBusy() {
        val busyStates = listOf(
            ImportProgress.Selected("content://x", ImportManager.MODE_PKG),
            ImportProgress.Validating("content://x", ImportManager.MODE_PKG),
            ImportProgress.ReadingMetadata("pkg", null),
            ImportProgress.CheckingStorage("id", 1L, 2L),
            ImportProgress.Extracting(0L, 100L, "f", "t"),
            ImportProgress.Copying(0L, 100L, "f", "t"),
            ImportProgress.Verifying("t"),
            ImportProgress.Registering("t"),
            ImportProgress.NeedPasscode("cid", "hint"),
            ImportProgress.NeedCopyConfirm("cid", "hint", 1, 1, 2, 3),
        )
        for (state in busyStates) {
            ImportManager.update(state)
            assertTrue("expected busy: $state", ImportManager.isBusy(state))
            assertFalse(ImportManager.tryBeginImport())
            ImportManager.reset()
        }
    }

    @Test
    fun installedAndFailedAreNotBusy() {
        ImportManager.update(ImportProgress.Installed("id", "Title"))
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
        ImportManager.update(
            ImportProgress.Failed(InstallErrorCode.MALFORMED_PACKAGE, "bad header"),
        )
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
    }

    @Test
    fun failedCarriesTypedCode() {
        ImportManager.update(
            ImportProgress.Failed(InstallErrorCode.INSUFFICIENT_STORAGE, "need 10 GiB"),
        )
        val failed = ImportManager.progress.value as ImportProgress.Failed
        assertEquals(InstallErrorCode.INSUFFICIENT_STORAGE, failed.code)
        assertEquals("need 10 GiB", failed.message)
    }

    @Test
    fun resetFreesSlotForNextImport() {
        assertTrue(ImportManager.tryBeginImport())
        ImportManager.update(
            ImportProgress.Copying(
                bytesCopied = 10L,
                totalBytes = 100L,
                currentFile = "eboot.bin",
                gameTitle = "Test",
            ),
        )
        assertTrue(ImportManager.isBusy())
        ImportManager.reset()
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
    }

    @Test
    fun batchSelectedIsBusy() {
        ImportManager.update(ImportProgress.BatchSelected("CUSA07023", "Bloodborne", 3))
        assertTrue(ImportManager.isBusy())
    }

    @Test
    fun batchExtractingIsBusy() {
        ImportManager.update(
            ImportProgress.BatchExtracting(1, 3, 500L, 1000L, "update.pkg", "Bloodborne"),
        )
        assertTrue(ImportManager.isBusy())
    }

    @Test
    fun batchInstalledIsNotBusy() {
        ImportManager.update(ImportProgress.BatchInstalled("CUSA07023", "Bloodborne", 3))
        assertFalse(ImportManager.isBusy())
    }

    @Test
    fun batchFailedIsNotBusy() {
        ImportManager.update(
            ImportProgress.BatchFailed(InstallErrorCode.MALFORMED_PACKAGE, "mismatch", 2, 3),
        )
        assertFalse(ImportManager.isBusy())
    }
}
