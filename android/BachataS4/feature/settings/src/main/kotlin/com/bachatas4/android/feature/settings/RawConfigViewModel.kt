package com.bachatas4.android.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.settings.Box64EnvironmentCodec
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeSettingCatalog
import com.bachatas4.android.runtime.settings.SettingKind
import com.bachatas4.android.runtime.settings.ShadPs4JsonCodec
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonPrimitive

data class RawConfigUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val shadPs4Json: String = "{}",
    val box64Environment: String = "",
    val validation: String? = null,
    val valid: Boolean = false,
)

@HiltViewModel
class RawConfigViewModel @Inject constructor(private val store: RuntimeProfileStore) : ViewModel() {
    private val catalog = RuntimeSettingCatalog.loadFromResources()
    private val mutableState = MutableStateFlow(RawConfigUiState())
    val state: StateFlow<RawConfigUiState> = mutableState

    fun load(scope: ProfileScope) {
        viewModelScope.launch {
            val profile = store.load(scope)
            mutableState.value = RawConfigUiState(
                scope = scope,
                shadPs4Json = renderShad(profile),
                box64Environment = renderBox64(profile),
            )
        }
    }

    fun editShadPs4(text: String) { mutableState.value = mutableState.value.copy(shadPs4Json = text, valid = false, validation = null) }
    fun editBox64(text: String) { mutableState.value = mutableState.value.copy(box64Environment = text, valid = false, validation = null) }

    fun validate(): Boolean = runCatching { applyDraft(RuntimeProfile()) }
        .fold(
            onSuccess = { mutableState.value = mutableState.value.copy(valid = true, validation = "Valid"); true },
            onFailure = { mutableState.value = mutableState.value.copy(valid = false, validation = it.message); false },
        )

    fun save() {
        if (!validate()) return
        viewModelScope.launch {
            runCatching { store.update(mutableState.value.scope, ::applyDraft) }
                .onSuccess { mutableState.value = mutableState.value.copy(validation = "Saved") }
                .onFailure { mutableState.value = mutableState.value.copy(valid = false, validation = it.message) }
        }
    }

    private fun applyDraft(profile: RuntimeProfile): RuntimeProfile {
        var updated = ShadPs4JsonCodec.applyRawJson(profile, mutableState.value.shadPs4Json, catalog.shadPs4)
        val raw = Box64EnvironmentCodec.decode(mutableState.value.box64Environment)
        val byKey = catalog.box64.associateBy { it.nativeKey }
        val values = updated.values.toMutableMap().apply { keys.removeAll(catalog.box64.map { it.id }.toSet()) }
        val unknown = linkedMapOf<String, String>()
        raw.forEach { (key, text) ->
            val spec = byKey[key]
            if (spec == null) unknown[key] = text else values[spec.id] = when (spec.kind) {
                SettingKind.BOOLEAN -> JsonPrimitive(text.toBooleanStrict())
                SettingKind.INTEGER -> JsonPrimitive(text.toLong())
                SettingKind.DECIMAL -> JsonPrimitive(text.toDouble())
                else -> JsonPrimitive(text)
            }
        }
        updated = updated.copy(values = values, unknownBox64 = unknown)
        return updated
    }

    private fun renderShad(profile: RuntimeProfile): String {
        var document = ShadPs4JsonCodec.empty().mergeUnknown(profile.unknownShadPs4)
        catalog.shadPs4.forEach { spec ->
            profile.values[spec.id]?.let { document = document.set(spec.section, spec.nativeKey.substringAfter('.'), it) }
        }
        return document.render()
    }

    private fun renderBox64(profile: RuntimeProfile): String {
        val values = profile.unknownBox64.toMutableMap()
        catalog.box64.forEach { spec ->
            (profile.values[spec.id] as? JsonPrimitive)?.let { values[spec.nativeKey] = it.content }
        }
        return Box64EnvironmentCodec.encode(values)
    }
}
