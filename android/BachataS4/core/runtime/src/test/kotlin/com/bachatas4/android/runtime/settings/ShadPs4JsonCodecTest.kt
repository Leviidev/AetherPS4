package com.bachatas4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ShadPs4JsonCodecTest {
    @Test
    fun typedEditPreservesUnknownNestedFields() {
        val source = """{"Future":{"nested":true},"GPU":{"null_gpu":false}}"""

        val edited = ShadPs4JsonCodec.parse(source)
            .set("GPU", "null_gpu", JsonPrimitive(true))
            .render()
        val root = ShadPs4JsonCodec.parse(edited).root

        assertTrue(root.getValue("Future").jsonObject.getValue("nested").jsonPrimitive.boolean)
        assertTrue(root.getValue("GPU").jsonObject.getValue("null_gpu").jsonPrimitive.boolean)
    }

    @Test
    fun malformedAndNonObjectRootsAreRejected() {
        assertThrows(IllegalArgumentException::class.java) { ShadPs4JsonCodec.parse("{") }
        assertThrows(IllegalArgumentException::class.java) { ShadPs4JsonCodec.parse("[]") }
    }

    @Test
    fun rawJsonMapsKnownValuesAndRetainsUnknownValues() {
        val spec = RuntimeSettingSpec(
            id = "gpu.null_gpu",
            nativeKey = "GPU.null_gpu",
            section = "GPU",
            category = "GPU",
            title = "Null GPU",
            help = "Disable rendering.",
            kind = SettingKind.BOOLEAN,
            defaultValue = JsonPrimitive(false),
        )

        val profile = ShadPs4JsonCodec.applyRawJson(
            RuntimeProfile(),
            """{"GPU":{"null_gpu":true,"future_gpu":7},"Future":{"value":"keep"}}""",
            listOf(spec),
        )

        assertEquals(JsonPrimitive(true), profile.values[spec.id])
        assertEquals(JsonPrimitive(7), profile.unknownShadPs4.getValue("GPU").jsonObject["future_gpu"])
        assertEquals("keep", profile.unknownShadPs4.getValue("Future").jsonObject["value"]?.jsonPrimitive?.content)
    }

    @Test
    fun rawJsonMapsNativeEnumOrdinalBackToProfileChoice() {
        val spec = RuntimeSettingSpec(
            id = "audio.audio_backend",
            nativeKey = "Audio.audio_backend",
            section = "Audio",
            kind = SettingKind.ENUM,
            defaultValue = JsonPrimitive("SDL"),
            choices = listOf("SDL", "OpenAL"),
            nativeEnumOrdinal = true,
        )

        val profile = ShadPs4JsonCodec.applyRawJson(
            RuntimeProfile(),
            """{"Audio":{"audio_backend":1}}""",
            listOf(spec),
        )

        assertEquals(JsonPrimitive("OpenAL"), profile.values[spec.id])
    }
}
