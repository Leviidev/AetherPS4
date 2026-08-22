package com.bachatas4.android.runtime.settings

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

@Serializable
data class RuntimeSettingSpec(
    val id: String,
    val nativeKey: String,
    val section: String = "",
    val category: String = "",
    val title: String = "",
    val help: String = "",
    val kind: SettingKind = SettingKind.STRING,
    val defaultValue: JsonElement? = null,
    val minimum: Double? = null,
    val maximum: Double? = null,
    val choices: List<String> = emptyList(),
    val nativeEnumOrdinal: Boolean = false,
    val scope: SettingScope = SettingScope.GLOBAL_AND_GAME,
    val restartRequired: Boolean = true,
    val risk: SettingRisk = SettingRisk.NORMAL,
    val readOnlyReason: String? = null,
) {
    init {
        require(!nativeEnumOrdinal || kind == SettingKind.ENUM) {
            "Native enum ordinal storage requires an enum setting"
        }
        require(!nativeEnumOrdinal || choices.isNotEmpty()) {
            "Native enum ordinal storage requires choices"
        }
    }
}

@Serializable
enum class SettingKind {
    BOOLEAN,
    ENUM,
    INTEGER,
    DECIMAL,
    STRING,
    PATH,
    LIST,
}

@Serializable
enum class SettingScope {
    GLOBAL_ONLY,
    GLOBAL_AND_GAME,
}

@Serializable
enum class SettingRisk {
    NORMAL,
    ADVANCED,
    DANGEROUS,
}
