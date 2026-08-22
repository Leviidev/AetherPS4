package com.bachatas4.android.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class InstallValidatorTest {
    @Test
    fun insufficientStorageWhenFreeBelowRequired() {
        assertEquals(
            InstallErrorCode.INSUFFICIENT_STORAGE,
            InstallValidator.checkStorage(requiredBytes = 100, freeBytes = 10),
        )
    }

    @Test
    fun storageOkWhenFreeEnough() {
        assertNull(InstallValidator.checkStorage(requiredBytes = 10, freeBytes = 100))
    }

    @Test
    fun alreadyInstalledWhenDestLaunchable() {
        assertEquals(
            InstallErrorCode.ALREADY_INSTALLED,
            InstallValidator.checkDestination(
                destExists = true,
                destLaunchable = true,
                destPartial = false,
            ),
        )
    }

    @Test
    fun partialExistsWhenIncomplete() {
        assertEquals(
            InstallErrorCode.PARTIAL_EXISTS,
            InstallValidator.checkDestination(
                destExists = true,
                destLaunchable = false,
                destPartial = true,
            ),
        )
    }
}
