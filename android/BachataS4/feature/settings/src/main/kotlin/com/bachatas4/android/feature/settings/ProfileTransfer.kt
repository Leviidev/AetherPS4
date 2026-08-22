package com.bachatas4.android.feature.settings

import com.bachatas4.android.runtime.settings.CURRENT_SCHEMA_VERSION
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeSettingSpec
import com.bachatas4.android.runtime.settings.SettingKind
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

object ProfileTransfer {
    private val json = Json { prettyPrint = true; encodeDefaults = true; ignoreUnknownKeys = false }

    fun export(profile: RuntimeProfile, specs: List<RuntimeSettingSpec>): String =
        json.encodeToString(sanitize(profile, specs)) + "\n"

    fun import(text: String, specs: List<RuntimeSettingSpec>, gameId: String? = null): RuntimeProfile {
        if (gameId != null) ProfileScope.Game(gameId)
        val profile = runCatching { json.decodeFromString<RuntimeProfile>(text) }
            .getOrElse { throw IllegalArgumentException("Invalid runtime profile JSON: ${it.message}", it) }
        require(profile.schemaVersion <= CURRENT_SCHEMA_VERSION) { "Profile schema is newer than supported" }
        require(profile.schemaVersion >= 0) { "Invalid profile schema" }
        return sanitize(profile.copy(schemaVersion = CURRENT_SCHEMA_VERSION), specs)
    }

    private fun sanitize(profile: RuntimeProfile, specs: List<RuntimeSettingSpec>): RuntimeProfile {
        val pathIds = specs.filter { it.kind == SettingKind.PATH }.mapTo(mutableSetOf()) { it.id }
        return profile.copy(
            values = profile.values.filterKeys { it !in pathIds },
            driverId = null,
            controllerSlots = emptyList(),
            touchLayoutId = null,
        )
    }
}
