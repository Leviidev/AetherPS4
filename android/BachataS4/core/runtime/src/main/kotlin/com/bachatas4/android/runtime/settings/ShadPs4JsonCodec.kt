package com.bachatas4.android.runtime.settings

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject

data class ShadPs4JsonDocument(val root: JsonObject) {
    fun set(section: String, key: String, value: JsonElement): ShadPs4JsonDocument {
        val group = (root[section] as? JsonObject).orEmpty().toMutableMap()
        group[key] = value
        return ShadPs4JsonDocument(JsonObject(root.toMutableMap().apply { put(section, JsonObject(group)) }))
    }

    fun mergeUnknown(values: Map<String, JsonElement>): ShadPs4JsonDocument {
        val merged = root.toMutableMap()
        values.forEach { (key, value) ->
            val currentObject = merged[key] as? JsonObject
            val incomingObject = value as? JsonObject
            merged[key] = if (currentObject != null && incomingObject != null) {
                JsonObject(currentObject + incomingObject)
            } else {
                value
            }
        }
        return ShadPs4JsonDocument(JsonObject(merged))
    }

    fun render(): String = ShadPs4JsonCodec.render(root)

    private fun JsonObject?.orEmpty(): JsonObject = this ?: JsonObject(emptyMap())
}

object ShadPs4JsonCodec {
    private val parser = Json { ignoreUnknownKeys = false }
    private val writer = Json { prettyPrint = true }

    fun parse(text: String): ShadPs4JsonDocument {
        val element = runCatching { parser.parseToJsonElement(text) }.getOrElse {
            throw IllegalArgumentException("Invalid shadPS4 JSON: ${it.message}", it)
        }
        require(element is JsonObject) { "shadPS4 config root must be a JSON object" }
        return ShadPs4JsonDocument(element)
    }

    fun empty(): ShadPs4JsonDocument = ShadPs4JsonDocument(JsonObject(emptyMap()))

    fun applyRawJson(
        profile: RuntimeProfile,
        text: String,
        specs: List<RuntimeSettingSpec>,
    ): RuntimeProfile {
        val root = parse(text).root
        val shadSpecs = specs.filter { it.section != "Box64" }
        val values = profile.values.toMutableMap().apply { keys.removeAll(shadSpecs.map { it.id }.toSet()) }
        val unknown = root.toMutableMap()
        shadSpecs.groupBy { it.section }.forEach { (section, sectionSpecs) ->
            val sectionElement = root[section] ?: return@forEach
            require(sectionElement is JsonObject) { "$section must be a JSON object" }
            val unknownSection = sectionElement.toMutableMap()
            sectionSpecs.forEach { spec ->
                val key = spec.nativeKey.substringAfter('.')
                sectionElement[key]?.let { nativeValue ->
                    values[spec.id] = if (spec.nativeEnumOrdinal) {
                        val ordinal = (nativeValue as? JsonPrimitive)?.intOrNull
                        require(ordinal != null && ordinal in spec.choices.indices) {
                            "Invalid native enum ordinal for ${spec.id}"
                        }
                        JsonPrimitive(spec.choices[ordinal])
                    } else {
                        nativeValue
                    }
                }
                unknownSection.remove(key)
            }
            if (unknownSection.isEmpty()) unknown.remove(section) else unknown[section] = JsonObject(unknownSection)
        }
        val updated = profile.copy(values = values, unknownShadPs4 = unknown)
        RuntimeProfileResolver(shadSpecs).resolve(updated, null)
        return updated
    }

    internal fun render(root: JsonObject): String = writer.encodeToString(root) + "\n"
}
