package com.bachatas4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Test

class GameIconPathsTest {
    @Test
    fun resolvesIconAndSfoUnderRelativePath() {
        val root = File("/tmp/app-files")
        assertEquals(
            File("/tmp/app-files/games/CUSA00900/sce_sys/icon0.png"),
            GameIconPaths.icon0(root, "games/CUSA00900"),
        )
        assertEquals(
            File("/tmp/app-files/games/CUSA00900/sce_sys/param.sfo"),
            GameIconPaths.paramSfo(root, "games/CUSA00900"),
        )
    }
}
