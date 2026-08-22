package com.bachatas4.android.data

/**
 * Returns (id, newTitle) pairs where on-disk SFO title differs from stored title.
 */
internal fun titlesToUpdate(
    games: List<Pair<String, String>>,
    sfoTitleFor: (String) -> String?,
): List<Pair<String, String>> =
    games.mapNotNull { (id, stored) ->
        val next = sfoTitleFor(id)?.trim()?.takeIf { it.isNotEmpty() } ?: return@mapNotNull null
        if (next == stored) null else id to next
    }
