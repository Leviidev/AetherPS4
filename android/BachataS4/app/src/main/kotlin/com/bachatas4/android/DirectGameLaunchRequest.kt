package com.bachatas4.android

import com.bachatas4.android.data.GameInstallVerifier
import java.io.File
import java.io.IOException

object DirectGameLaunchRequest {
    const val EXTRA_GAME_ID = "game_id"

    sealed interface Resolution {
        data class Ready(val gameId: String) : Resolution
        data class Rejected(val reason: String) : Resolution
    }

    fun resolve(filesDir: File, rawGameId: String?): Resolution {
        val gameId = rawGameId?.trim().orEmpty()
        if (!TITLE_ID.matches(gameId)) {
            return Resolution.Rejected("game_id must match AAAA00000")
        }

        val gamesRoot: File
        val gameRoot: File
        try {
            gamesRoot = File(filesDir, "games").canonicalFile
            gameRoot = File(gamesRoot, gameId).canonicalFile
        } catch (error: IOException) {
            return Resolution.Rejected("Unable to resolve imported game: ${error.message}")
        } catch (error: SecurityException) {
            return Resolution.Rejected("Unable to access imported game: ${error.message}")
        }

        if (!gameRoot.toPath().startsWith(gamesRoot.toPath())) {
            return Resolution.Rejected("Imported game path escapes app storage")
        }
        if (!GameInstallVerifier.canLaunch(filesDir, "games/$gameId")) {
            return Resolution.Rejected("Imported game $gameId is not fully installed")
        }
        return Resolution.Ready(gameId)
    }

    private val TITLE_ID = Regex("^[A-Z]{4}[0-9]{5}$")
}
