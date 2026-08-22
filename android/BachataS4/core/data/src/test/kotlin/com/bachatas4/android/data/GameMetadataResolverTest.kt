package com.bachatas4.android.data

import org.junit.Assert.assertEquals
import org.junit.Test

class GameMetadataResolverTest {
    @Test
    fun prefersSfoTitleAndTitleId() {
        val resolved = GameMetadataResolver.resolve(
            folderName = "some-folder-CUSA00001",
            sfo = ParamSfoMetadata(title = "Real Title", titleId = "CUSA00900"),
        )
        assertEquals("CUSA00900", resolved.id)
        assertEquals("Real Title", resolved.title)
    }

    @Test
    fun fallsBackToFolderNameAndCusaRegex() {
        val resolved = GameMetadataResolver.resolve(
            folderName = "Bloodborne-CUSA00900",
            sfo = null,
        )
        assertEquals("CUSA00900", resolved.id)
        assertEquals("Bloodborne-CUSA00900", resolved.title)
    }

    @Test
    fun fallsBackToImportedGameAndGeneratedId() {
        val resolved = GameMetadataResolver.resolve(
            folderName = null,
            sfo = ParamSfoMetadata(null, null),
            randomId = { "fixed-uuid" },
        )
        assertEquals("GAME-fixed-uuid", resolved.id)
        assertEquals("Imported game", resolved.title)
    }

    @Test
    fun rejectsUnsafeTitleId() {
        val resolved = GameMetadataResolver.resolve(
            folderName = "safe-CUSA12345",
            sfo = ParamSfoMetadata(title = "X", titleId = "../escape"),
            randomId = { "u" },
        )
        assertEquals("CUSA12345", resolved.id)
        assertEquals("X", resolved.title)
    }
}
