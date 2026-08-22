package com.bachatas4.android.feature.settings

import com.bachatas4.android.runtime.settings.RuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.settings.RuntimeSettingSpec
import com.bachatas4.android.runtime.settings.SettingKind
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertFalse
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ProfileTransferTest {
    private val path = RuntimeSettingSpec(id = "paths.data", nativeKey = "Paths.data", kind = SettingKind.PATH)

    @Test
    fun exportOmitsDriverAndPathValues() {
        val text = ProfileTransfer.export(
            RuntimeProfile(values = mapOf(path.id to JsonPrimitive("/private/path"), "gpu.value" to JsonPrimitive(true)), driverId = "turnip-secret"),
            listOf(path),
        )
        assertFalse(text.contains("/private/path"))
        assertFalse(text.contains("turnip-secret"))
        assertTrue(text.contains("gpu.value"))
    }

    @Test
    fun importRejectsFutureSchemaAndInvalidGameId() {
        assertThrows(IllegalArgumentException::class.java) { ProfileTransfer.import("""{"schemaVersion":999}""", emptyList()) }
        assertThrows(IllegalArgumentException::class.java) { ProfileTransfer.import("""{"schemaVersion":1}""", emptyList(), "../bad") }
    }

    @Test
    fun backendSelectionRoundTrips() {
        val exported = ProfileTransfer.export(
            RuntimeProfile(guestBackend = RuntimeGuestBackend.BOX64),
            emptyList(),
        )

        val imported = ProfileTransfer.import(exported, emptyList())

        assertEquals(RuntimeGuestBackend.BOX64, imported.guestBackend)
    }

    @Test
    fun olderProfileWithoutBackendRemainsInheritable() {
        val imported = ProfileTransfer.import("""{"schemaVersion":1}""", emptyList())

        assertEquals(null, imported.guestBackend)
    }
}
