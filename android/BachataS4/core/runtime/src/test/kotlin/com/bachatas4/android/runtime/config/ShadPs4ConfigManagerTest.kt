package com.bachatas4.android.runtime.config

import com.bachatas4.android.runtime.settings.RuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeProfileResolver
import com.bachatas4.android.runtime.settings.RuntimeSettingSpec
import com.bachatas4.android.runtime.settings.SettingKind
import java.nio.file.Files
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ShadPs4ConfigManagerTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun enablesPersistentPipelineCacheWithoutReplacingExistingSettings() {
        val runtimeRoot = temporaryFolder.newFolder("runtime").toPath()
        val config = runtimeRoot.resolve(".local/share/shadPS4/config.json")
        Files.createDirectories(config.parent)
        Files.writeString(
            config,
            """{"General":{"console_language":1},"Vulkan":{"gpu_id":3,"pipeline_cache_enabled":false}}""",
        )

        ShadPs4ConfigManager.applyAndroidCompatibilityProfile(runtimeRoot)

        val root = Json.parseToJsonElement(Files.readString(config)).jsonObject
        assertTrue(root.getValue("General").jsonObject.getValue("console_language").jsonPrimitive.content == "1")
        val vulkan = root.getValue("Vulkan").jsonObject
        assertTrue(vulkan.getValue("gpu_id").jsonPrimitive.content == "3")
        assertTrue(vulkan.getValue("pipeline_cache_enabled").jsonPrimitive.boolean)
        assertFalse(vulkan.getValue("pipeline_cache_archived").jsonPrimitive.boolean)
        assertFalse(vulkan.getValue("vkvalidation_core_enabled").jsonPrimitive.boolean)
        assertFalse(root.getValue("General").jsonObject.getValue("dev_kit_mode").jsonPrimitive.boolean)
        assertFalse(root.getValue("Log").jsonObject.getValue("sync").jsonPrimitive.boolean)
    }

    @Test
    fun createsMinimalConfigWhenShadPs4HasNotRun() {
        val runtimeRoot = temporaryFolder.newFolder("runtime-new").toPath()

        ShadPs4ConfigManager.applyAndroidCompatibilityProfile(runtimeRoot)

        val config = runtimeRoot.resolve(".local/share/shadPS4/config.json")
        val vulkan = Json.parseToJsonElement(Files.readString(config)).jsonObject
            .getValue("Vulkan").jsonObject
        assertTrue(vulkan.getValue("pipeline_cache_enabled").jsonPrimitive.boolean)
    }

    @Test
    fun writesResolvedSettingsWithoutReplacingUnknownConfig() {
        val runtimeRoot = temporaryFolder.newFolder("runtime-resolved").toPath()
        val config = runtimeRoot.resolve(".local/share/shadPS4/config.json")
        Files.createDirectories(config.parent)
        Files.writeString(config, """{"Future":{"keep":true},"GPU":{"null_gpu":false}}""")
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
        val resolved = RuntimeProfileResolver(listOf(spec)).resolve(
            RuntimeProfile(values = mapOf(spec.id to JsonPrimitive(true))),
            null,
        )

        ShadPs4ConfigManager.write(runtimeRoot, resolved)

        val root = Json.parseToJsonElement(Files.readString(config)).jsonObject
        assertTrue(root.getValue("Future").jsonObject.getValue("keep").jsonPrimitive.boolean)
        assertTrue(root.getValue("GPU").jsonObject.getValue("null_gpu").jsonPrimitive.boolean)
    }

    @Test
    fun writesNativeOrdinalForNumericEnumSettings() {
        val runtimeRoot = temporaryFolder.newFolder("runtime-enum").toPath()
        val spec = RuntimeSettingSpec(
            id = "audio.audio_backend",
            nativeKey = "Audio.audio_backend",
            section = "Audio",
            kind = SettingKind.ENUM,
            defaultValue = JsonPrimitive("SDL"),
            choices = listOf("SDL", "OpenAL"),
            nativeEnumOrdinal = true,
        )
        val resolved = RuntimeProfileResolver(listOf(spec)).resolve(
            RuntimeProfile(values = mapOf(spec.id to JsonPrimitive("OpenAL"))),
            null,
        )

        ShadPs4ConfigManager.write(runtimeRoot, resolved)

        val config = runtimeRoot.resolve(".local/share/shadPS4/config.json")
        val audioBackend = Json.parseToJsonElement(Files.readString(config)).jsonObject
            .getValue("Audio").jsonObject.getValue("audio_backend").jsonPrimitive.int
        assertTrue(audioBackend == 1)
    }
}
