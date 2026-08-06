package com.bachatas4.android.runtime.settings

import com.bachatas4.android.runtime.input.ControllerProfile
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull

data class CompatibilityConstraint(
    val value: JsonElement,
    val reason: String,
) {
    init {
        require(reason.isNotBlank()) { "Compatibility constraint reason is blank" }
    }
}

enum class ValueSource {
    DEFAULT,
    GLOBAL,
    GAME,
    COMPATIBILITY,
}

data class ResolvedSetting(
    val spec: RuntimeSettingSpec,
    val value: JsonElement?,
    val source: ValueSource,
    val compatibilityReason: String? = null,
)

data class ResolvedRuntimeProfile(
    val schemaVersion: Int,
    val settings: Map<String, ResolvedSetting>,
    val unknownShadPs4: Map<String, JsonElement>,
    val unknownBox64: Map<String, String>,
    val guestBackend: RuntimeGuestBackend,
    val box64Preset: Box64Preset,
    val driverId: String,
    val controllerSlots: List<ControllerProfile>,
    val touchLayoutId: String?,
    /** Mali freeflight staging path; default false (Turnip mainline). */
    val maliGpuOptimizations: Boolean = false,
) {
    fun boolean(id: String): Boolean =
        requireNotNull(settings[id]?.value as? JsonPrimitive) { "Missing boolean setting $id" }.boolean
}

class RuntimeProfileResolver(
    specs: List<RuntimeSettingSpec>,
    private val compatibilityConstraints: Map<String, CompatibilityConstraint> = emptyMap(),
) {
    private val specsById = specs.associateBy { it.id }.also {
        require(it.size == specs.size) { "Duplicate runtime setting id" }
    }

    init {
        compatibilityConstraints.forEach { (id, constraint) ->
            val spec = requireNotNull(specsById[id]) { "Unknown compatibility setting $id" }
            validateValue(spec, constraint.value)
        }
    }

    fun resolve(global: RuntimeProfile, game: RuntimeProfile?): ResolvedRuntimeProfile {
        validateProfile(global)
        game?.let(::validateProfile)
        val settings = specsById.mapValues { (id, spec) ->
            val compatibility = compatibilityConstraints[id]
            when {
                compatibility != null -> ResolvedSetting(
                    spec,
                    compatibility.value,
                    ValueSource.COMPATIBILITY,
                    compatibility.reason,
                )
                game?.values?.containsKey(id) == true -> ResolvedSetting(spec, game.values.getValue(id), ValueSource.GAME)
                global.values.containsKey(id) -> ResolvedSetting(spec, global.values.getValue(id), ValueSource.GLOBAL)
                else -> ResolvedSetting(spec, spec.defaultValue, ValueSource.DEFAULT)
            }
        }
        return ResolvedRuntimeProfile(
            schemaVersion = CURRENT_SCHEMA_VERSION,
            settings = settings,
            unknownShadPs4 = global.unknownShadPs4 + game.orEmptyUnknownShadPs4(),
            unknownBox64 = global.unknownBox64 + game.orEmptyUnknownBox64(),
            guestBackend = resolveGuestBackend(global, game),
            box64Preset = game?.box64Preset ?: global.box64Preset ?: Box64Preset.DEFAULT,
            driverId = game?.driverId ?: global.driverId ?: "system",
            controllerSlots = game?.controllerSlots?.takeIf { it.isNotEmpty() } ?: global.controllerSlots,
            touchLayoutId = game?.touchLayoutId ?: global.touchLayoutId,
            maliGpuOptimizations = game?.maliGpuOptimizations ?: global.maliGpuOptimizations ?: false,
        )
    }

    private fun validateProfile(profile: RuntimeProfile) {
        require(profile.schemaVersion in 0..CURRENT_SCHEMA_VERSION) {
            "Unsupported runtime profile schema ${profile.schemaVersion}"
        }
        profile.values.forEach { (id, value) -> specsById[id]?.let { validateValue(it, value) } }
    }

    private fun validateValue(spec: RuntimeSettingSpec, value: JsonElement) {
        val primitive = value as? JsonPrimitive
        val valid = when (spec.kind) {
            SettingKind.BOOLEAN -> primitive?.booleanOrNull != null
            SettingKind.INTEGER -> primitive?.longOrNull != null
            SettingKind.DECIMAL -> primitive?.doubleOrNull != null
            SettingKind.STRING, SettingKind.PATH -> primitive?.isString == true
            SettingKind.ENUM -> primitive?.isString == true && primitive.content in spec.choices
            SettingKind.LIST -> value is JsonArray
        }
        require(valid) { "Invalid value for ${spec.id}" }
        val number = primitive?.doubleOrNull
        require(number == null || spec.minimum == null || number >= spec.minimum) { "Value below minimum for ${spec.id}" }
        require(number == null || spec.maximum == null || number <= spec.maximum) { "Value above maximum for ${spec.id}" }
    }

    private fun RuntimeProfile?.orEmptyUnknownShadPs4(): Map<String, JsonElement> = this?.unknownShadPs4.orEmpty()
    private fun RuntimeProfile?.orEmptyUnknownBox64(): Map<String, String> = this?.unknownBox64.orEmpty()

    companion object {
        fun resolveGuestBackend(global: RuntimeProfile, game: RuntimeProfile?): RuntimeGuestBackend =
            game?.guestBackend ?: global.guestBackend ?: RuntimeGuestBackend.FEX
    }
}
