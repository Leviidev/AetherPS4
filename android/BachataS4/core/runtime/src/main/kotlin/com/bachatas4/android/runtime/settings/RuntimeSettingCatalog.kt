package com.bachatas4.android.runtime.settings

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull

data class RuntimeSettingCatalog(
    val shadPs4: List<RuntimeSettingSpec>,
    val box64: List<RuntimeSettingSpec>,
) {
    companion object {
        private val json = Json { ignoreUnknownKeys = false }

        fun parse(text: String): List<RuntimeSettingSpec> {
            val specs = json.decodeFromString<List<RuntimeSettingSpec>>(text)
            validate(specs)
            return specs
        }

        fun loadFromResources(): RuntimeSettingCatalog = RuntimeSettingCatalog(
            shadPs4 = loadResource("/runtime-settings/shadps4.json"),
            box64 = loadResource("/runtime-settings/box64.json"),
        )

        private fun loadResource(path: String): List<RuntimeSettingSpec> {
            val text = requireNotNull(RuntimeSettingCatalog::class.java.getResourceAsStream(path)) {
                "Missing runtime setting catalog $path"
            }.bufferedReader().use { it.readText() }
            return parse(text)
        }

        private fun validate(specs: List<RuntimeSettingSpec>) {
            val ids = mutableSetOf<String>()
            val nativeKeys = mutableSetOf<String>()
            specs.forEach { spec ->
                require(spec.id.isNotBlank()) { "setting id is blank" }
                require(spec.nativeKey.isNotBlank()) { "nativeKey is blank for ${spec.id}" }
                require(ids.add(spec.id)) { "duplicate id ${spec.id}" }
                require(nativeKeys.add(spec.nativeKey)) { "duplicate nativeKey ${spec.nativeKey}" }
                require(spec.minimum == null || spec.maximum == null || spec.minimum <= spec.maximum) {
                    "invalid range for ${spec.id}"
                }
                require(spec.kind != SettingKind.ENUM || spec.choices.isNotEmpty()) {
                    "enum ${spec.id} has no choices"
                }
                require(spec.readOnlyReason == null || spec.readOnlyReason.isNotBlank()) {
                    "read-only setting ${spec.id} has no reason"
                }
                validateDefault(spec)
            }
        }

        private fun validateDefault(spec: RuntimeSettingSpec) {
            val value = spec.defaultValue ?: return
            val primitive = value as? JsonPrimitive
            val validType = when (spec.kind) {
                SettingKind.BOOLEAN -> primitive?.booleanOrNull != null
                SettingKind.INTEGER -> primitive?.longOrNull != null
                SettingKind.DECIMAL -> primitive?.doubleOrNull != null
                SettingKind.STRING, SettingKind.PATH, SettingKind.ENUM -> primitive?.isString == true
                SettingKind.LIST -> value is JsonArray
            }
            require(validType) { "default type does not match ${spec.kind} for ${spec.id}" }
            if (spec.kind == SettingKind.ENUM) {
                require(primitive!!.content in spec.choices) { "default choice is invalid for ${spec.id}" }
            }
            val numeric = primitive?.doubleOrNull
            require(numeric == null || spec.minimum == null || numeric >= spec.minimum) {
                "default range is invalid for ${spec.id}"
            }
            require(numeric == null || spec.maximum == null || numeric <= spec.maximum) {
                "default range is invalid for ${spec.id}"
            }
        }
    }
}
