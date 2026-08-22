package com.bachatas4.android.data

import org.junit.Assert.assertEquals
import org.junit.Test

class GameTitleBackfillTest {
    @Test
    fun onlyReturnsChangedTitles() {
        val updates = titlesToUpdate(
            games = listOf(
                "CUSA1" to "folder-name",
                "CUSA2" to "Already Good",
                "CUSA3" to "old",
            ),
            sfoTitleFor = { id ->
                when (id) {
                    "CUSA1" -> "Real Name"
                    "CUSA2" -> "Already Good"
                    "CUSA3" -> null
                    else -> null
                }
            },
        )
        assertEquals(listOf("CUSA1" to "Real Name"), updates)
    }
}
