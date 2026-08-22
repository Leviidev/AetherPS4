package com.bachatas4.android.data

import com.bachatas4.android.runtime.settings.ProfileScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class LegacyRuntimeSettingsMigrationTest {
    @get:Rule val temporaryFolder = TemporaryFolder()

    @Test
    fun bundledDriverMigratesToSystemOnceWithoutDeletingLegacyData() = runTest {
        var reads = 0
        val store = RuntimeProfileStore(temporaryFolder.root)
        val migration = LegacyRuntimeSettingsMigration(
            temporaryFolder.root,
            store,
            LegacyDriverSettings { reads++; "TURNIP_26_1_0" },
        )
        assertTrue(migration.migrate())
        assertNull(store.load(ProfileScope.Global).driverId)
        assertFalse(migration.migrate())
        assertEquals(1, reads)
        assertTrue(temporaryFolder.root.resolve("settings/backups/global-before-legacy-migration.json").isFile)
    }

    @Test
    fun validatedCustomDriverCanMigrateWhileInvalidFallsBackToSystem() = runTest {
        val validRoot = temporaryFolder.newFolder("valid")
        val validStore = RuntimeProfileStore(validRoot)
        LegacyRuntimeSettingsMigration(validRoot, validStore, LegacyDriverSettings { "CUSTOM" }) { "turnip-0123456789abcdef" }.migrate()
        assertEquals("turnip-0123456789abcdef", validStore.load(ProfileScope.Global).driverId)

        val invalidRoot = temporaryFolder.newFolder("invalid")
        val invalidStore = RuntimeProfileStore(invalidRoot)
        LegacyRuntimeSettingsMigration(invalidRoot, invalidStore, LegacyDriverSettings { "CUSTOM" }).migrate()
        assertNull(invalidStore.load(ProfileScope.Global).driverId)
    }
}
