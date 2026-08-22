package com.bachatas4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RuntimeProfileResolverTest {
    private val nullGpu = RuntimeSettingSpec(
        id = "gpu.null_gpu",
        nativeKey = "GPU.null_gpu",
        section = "GPU",
        category = "GPU",
        title = "Null GPU",
        help = "Disable rendering.",
        kind = SettingKind.BOOLEAN,
        defaultValue = JsonPrimitive(false),
    )

    @Test
    fun gameOverrideWinsAndResetInheritsGlobal() {
        val resolver = RuntimeProfileResolver(listOf(nullGpu))
        val global = RuntimeProfile(values = mapOf(nullGpu.id to JsonPrimitive(true)))
        val game = RuntimeProfile(values = mapOf(nullGpu.id to JsonPrimitive(false)))

        val overridden = resolver.resolve(global, game)
        assertFalse(overridden.boolean(nullGpu.id))
        assertEquals(ValueSource.GAME, overridden.settings.getValue(nullGpu.id).source)

        val reset = resolver.resolve(global, game.copy(values = emptyMap()))
        assertTrue(reset.boolean(nullGpu.id))
        assertEquals(ValueSource.GLOBAL, reset.settings.getValue(nullGpu.id).source)
    }

    @Test
    fun compatibilityConstraintWinsWithoutMutatingProfiles() {
        val resolver = RuntimeProfileResolver(
            listOf(nullGpu),
            mapOf(nullGpu.id to CompatibilityConstraint(JsonPrimitive(false), "Required on Android")),
        )
        val global = RuntimeProfile(values = mapOf(nullGpu.id to JsonPrimitive(true)))

        val resolved = resolver.resolve(global, null)

        assertFalse(resolved.boolean(nullGpu.id))
        assertEquals(ValueSource.COMPATIBILITY, resolved.settings.getValue(nullGpu.id).source)
        assertTrue((global.values.getValue(nullGpu.id) as JsonPrimitive).jsonPrimitiveBoolean())
    }

    @Test
    fun invalidKnownValueIsRejectedWithSettingId() {
        val resolver = RuntimeProfileResolver(listOf(nullGpu))

        val error = assertThrows(IllegalArgumentException::class.java) {
            resolver.resolve(RuntimeProfile(values = mapOf(nullGpu.id to JsonPrimitive("bad"))), null)
        }

        assertTrue(error.message.orEmpty().contains(nullGpu.id))
    }

    @Test
    fun box64PresetSupportsGlobalAndGameOverride() {
        val resolver = RuntimeProfileResolver(listOf(nullGpu))
        val global = RuntimeProfile(box64Preset = Box64Preset.FAST)

        assertEquals(Box64Preset.FAST, resolver.resolve(global, RuntimeProfile()).box64Preset)
        assertEquals(
            Box64Preset.CUSTOM,
            resolver.resolve(global, RuntimeProfile(box64Preset = Box64Preset.CUSTOM)).box64Preset,
        )
    }

    @Test
    fun guestBackendDefaultsToFex() {
        val resolved = RuntimeProfileResolver(listOf(nullGpu)).resolve(RuntimeProfile(), null)

        assertEquals(RuntimeGuestBackend.FEX, resolved.guestBackend)
    }

    @Test
    fun globalGuestBackendOverridesProductDefault() {
        val global = RuntimeProfile(guestBackend = RuntimeGuestBackend.BOX64)

        val resolved = RuntimeProfileResolver(listOf(nullGpu)).resolve(global, null)

        assertEquals(RuntimeGuestBackend.BOX64, resolved.guestBackend)
    }

    @Test
    fun gameGuestBackendOverridesGlobalBackend() {
        val global = RuntimeProfile(guestBackend = RuntimeGuestBackend.BOX64)
        val game = RuntimeProfile(guestBackend = RuntimeGuestBackend.FEX)

        val resolved = RuntimeProfileResolver(listOf(nullGpu)).resolve(global, game)

        assertEquals(RuntimeGuestBackend.FEX, resolved.guestBackend)
    }

    @Test
    fun nullGameGuestBackendInheritsGlobalBackend() {
        val global = RuntimeProfile(guestBackend = RuntimeGuestBackend.BOX64)

        val resolved = RuntimeProfileResolver(listOf(nullGpu)).resolve(global, RuntimeProfile())

        assertEquals(RuntimeGuestBackend.BOX64, resolved.guestBackend)
    }

    private fun JsonPrimitive.jsonPrimitiveBoolean(): Boolean = content.toBooleanStrict()
}
