package com.bachatas4.android.data

import java.util.UUID

data class ResolvedGameMetadata(
    val id: String,
    val title: String,
    val subtitle: String?,
    val detail: String?,
)

object GameMetadataResolver {
    private val safeId = Regex("^[A-Za-z0-9._-]+$")
    private val cusa = Regex("CUSA\\d{5}", RegexOption.IGNORE_CASE)

    fun resolve(
        folderName: String?,
        sfo: ParamSfoMetadata?,
        randomId: () -> String = { UUID.randomUUID().toString() },
    ): ResolvedGameMetadata {
        val title = sfo?.title?.trim()?.takeIf { it.isNotEmpty() }
            ?: folderName?.trim()?.takeIf { it.isNotEmpty() }
            ?: "Imported game"
        val id = sfo?.titleId?.trim()?.takeIf { it.isNotEmpty() && safeId.matches(it) }
            ?: folderName?.let { cusa.find(it)?.value?.uppercase() }
            ?: "GAME-${randomId()}"
        return ResolvedGameMetadata(
            id = id,
            title = title,
            subtitle = sfo?.subtitle?.trim()?.takeIf { it.isNotEmpty() },
            detail = sfo?.detail?.trim()?.takeIf { it.isNotEmpty() },
        )
    }
}
