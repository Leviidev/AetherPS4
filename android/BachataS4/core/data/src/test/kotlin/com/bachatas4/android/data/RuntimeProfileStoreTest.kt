package com.bachatas4.android.data

import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import java.io.File
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RuntimeProfileStoreTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun updatePersistsGlobalAndGameProfilesSeparately() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) {
            it.copy(values = mapOf("gpu.null_gpu" to JsonPrimitive(true)))
        }
        store.update(ProfileScope.Game("CUSA00001")) {
            it.copy(values = mapOf("gpu.null_gpu" to JsonPrimitive(false)))
        }

        assertEquals(JsonPrimitive(true), store.load(ProfileScope.Global).values["gpu.null_gpu"])
        assertEquals(JsonPrimitive(false), store.load(ProfileScope.Game("CUSA00001")).values["gpu.null_gpu"])
        assertTrue(File(temporaryFolder.root, "settings/global.json").isFile)
        assertTrue(File(temporaryFolder.root, "settings/games/CUSA00001.json").isFile)
    }

    @Test
    fun invalidGameIdCannotEscapeSettingsDirectory() {
        val store = RuntimeProfileStore(temporaryFolder.root)

        assertThrows(IllegalArgumentException::class.java) {
            runTest { store.load(ProfileScope.Game("../escape")) }
        }
        assertFalse(File(temporaryFolder.root, "escape.json").exists())
    }

    @Test
    fun importRejectsFutureSchemaAndPreservesCurrentProfile() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(driverId = "system") }

        val error = runCatching {
            store.import(ProfileScope.Global, """{"schemaVersion":999,"driverId":"future"}""")
        }.exceptionOrNull()

        assertTrue(error is IllegalArgumentException)
        assertEquals("system", store.load(ProfileScope.Global).driverId)
    }

    @Test
    fun loadIgnoresUnknownKeysFromOlderProfileSchema() = runTest {
        File(temporaryFolder.root, "settings").mkdirs()
        File(temporaryFolder.root, "settings/global.json").writeText(
            """{"schemaVersion":1,"driverId":"turnip-test","maliGpuOptimizations":true}""",
        )

        val store = RuntimeProfileStore(temporaryFolder.root)
        val loaded = store.load(ProfileScope.Global)

        assertEquals("turnip-test", loaded.driverId)
    }

    @Test
    fun exportRoundTripsUnknownFields() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val profile = RuntimeProfile(
            unknownShadPs4 = mapOf("Future" to JsonPrimitive("value")),
            unknownBox64 = mapOf("BOX64_FUTURE" to "1"),
        )
        store.update(ProfileScope.Global) { profile }

        val exported = store.export(ProfileScope.Global)
        val importedRoot = temporaryFolder.newFolder("imported")
        val importedStore = RuntimeProfileStore(importedRoot)
        importedStore.import(ProfileScope.Global, exported)

        assertEquals(profile, importedStore.load(ProfileScope.Global))
    }

    @Test
    fun persistsGlobalAndGameBackendSelectionsSeparately() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(guestBackend = RuntimeGuestBackend.FEX) }
        store.update(ProfileScope.Game("CUSA00900")) {
            it.copy(guestBackend = RuntimeGuestBackend.BOX64)
        }

        assertEquals(RuntimeGuestBackend.FEX, store.load(ProfileScope.Global).guestBackend)
        assertEquals(
            RuntimeGuestBackend.BOX64,
            store.load(ProfileScope.Game("CUSA00900")).guestBackend,
        )
    }
}
