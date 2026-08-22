package com.bachatas4.android.data

import android.content.Context
import com.bachatas4.android.database.GameDao
import com.bachatas4.android.database.GameEntity
import com.bachatas4.android.model.Game
import java.io.File
import javax.inject.Inject
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

class GameRepository @Inject constructor(
    private val gameDao: GameDao,
    @ApplicationContext private val context: Context,
) {
    fun observeGames(): Flow<List<Game>> =
        gameDao.observeAll().map { games -> games.map { it.toModel() } }

    suspend fun getGame(id: String): Game? = gameDao.getById(id)?.toModel()

    suspend fun addImportedGame(
        result: ContentImportResult,
        sourceUri: String,
        importedAtMs: Long,
    ) {
        gameDao.insert(
            GameEntity(
                id = result.game.id,
                title = result.game.title,
                relativePath = result.game.relativePath,
                sourceUri = sourceUri,
                importedAtMs = importedAtMs,
                subtitle = result.game.subtitle,
                detail = result.game.detail,
                lastLaunchedAtMs = 0L,
            ),
        )
    }

    /**
     * Reconcile on-disk games with the database using the same verify+manifest path
     * as PKG/folder install. Incomplete trees are never registered.
     */
    suspend fun syncLibrary() {
        val gamesRoot = context.filesDir.resolve("games")
        if (!gamesRoot.isDirectory) return

        val dbGames = gameDao.getAll()
        val dbIds = dbGames.map { it.id }.toSet()
        val plan = LibrarySync.planSync(
            gamesRoot = gamesRoot,
            dbIds = dbIds,
            importBusy = ImportManager.isBusy(),
            filesDir = context.filesDir,
        )
        LibrarySync.applyHeals(plan)
        for (insert in plan.inserts) {
            // Re-check after heal so canLaunch / manifest are present
            if (!GameInstallVerifier.canLaunch(context.filesDir, insert.relativePath) &&
                GameInstallVerifier.verifyTreeForRegistration(
                    File(context.filesDir, insert.relativePath),
                    insert.id,
                ) is GameInstallVerifier.VerifyResult.Ok
            ) {
                // Heal should have written manifest; re-read canLaunch after applyHeals
            }
            if (GameInstallVerifier.canLaunch(context.filesDir, insert.relativePath) ||
                GameInstallVerifier.requiredFilesPresent(File(context.filesDir, insert.relativePath))
            ) {
                // Ensure manifest before insert
                val dir = File(context.filesDir, insert.relativePath)
                if (InstallManifestIo.read(dir) == null) {
                    val verify = GameInstallVerifier.verifyTreeForRegistration(dir, insert.id)
                    if (verify is GameInstallVerifier.VerifyResult.Ok) {
                        InstallManifestIo.write(
                            dir,
                            InstallManifest(
                                status = InstallManifestIo.STATUS_INSTALLED,
                                gameId = insert.id,
                                contentId = null,
                                mode = ImportManager.MODE_FOLDER,
                                sourceUri = insert.sourceUri,
                                installedAtMs = System.currentTimeMillis(),
                                requiredFiles = GameInstallVerifier.REQUIRED_FILES,
                                bytesTotal = verify.bytesTotal,
                            ),
                        )
                    } else {
                        continue
                    }
                }
                if (insert.id !in dbIds) {
                    gameDao.insert(
                        GameEntity(
                            id = insert.id,
                            title = insert.title,
                            relativePath = insert.relativePath,
                            sourceUri = insert.sourceUri,
                            importedAtMs = System.currentTimeMillis(),
                            subtitle = insert.subtitle,
                            detail = insert.detail,
                            lastLaunchedAtMs = 0L,
                        ),
                    )
                }
            }
        }
        for (id in plan.dbIdsToDrop) {
            // Only drop when folder missing or not launchable and not complete
            val dir = File(gamesRoot, id)
            if (!dir.isDirectory || !GameInstallVerifier.canLaunch(context.filesDir, "games/$id")) {
                if (!dir.isDirectory ||
                    GameInstallVerifier.verifyTreeForRegistration(dir, id)
                        is GameInstallVerifier.VerifyResult.Fail
                ) {
                    runCatching { gameDao.deleteById(id) }
                }
            }
        }
    }

    /** @deprecated Use [syncLibrary]. Kept for call-site compatibility. */
    suspend fun syncOrphanedFolders() = syncLibrary()

    /**
     * Re-read TITLE from on-disk param.sfo for games that were imported under folder names.
     * Does not rename game ids or directories.
     */
    suspend fun backfillTitlesFromSfo() {
        val entities = gameDao.getAll()
        val byId = entities.associateBy { it.id }
        val updates = titlesToUpdate(
            games = entities.map { it.id to it.title },
            sfoTitleFor = { id ->
                val entity = byId[id] ?: return@titlesToUpdate null
                val file = GameIconPaths.paramSfo(context.filesDir, entity.relativePath)
                if (!file.isFile) return@titlesToUpdate null
                runCatching { ParamSfoReader.parse(file.readBytes()).title }.getOrNull()
            },
        )
        updates.forEach { (id, title) -> gameDao.updateTitle(id, title) }
    }

    suspend fun updateLastLaunched(id: String) {
        gameDao.updateLastLaunched(id, System.currentTimeMillis())
    }

    /**
     * Re-resolve metadata from on-disk param.sfo and bump importedAtMs after an
     * overlay install. No-op if the game row is missing.
     */
    suspend fun touchGame(gameId: String) {
        val entity = gameDao.getById(gameId) ?: return
        val sfoFile = GameIconPaths.paramSfo(context.filesDir, entity.relativePath)
        val sfo = if (sfoFile.isFile) {
            runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
        } else null
        gameDao.updateMetadata(
            id = gameId,
            title = sfo?.title?.takeIf { it.isNotBlank() } ?: entity.title,
            subtitle = sfo?.subtitle ?: entity.subtitle,
            detail = sfo?.detail ?: entity.detail,
            importedAtMs = System.currentTimeMillis(),
        )
    }

    suspend fun deleteGame(id: String): Boolean {
        val game = gameDao.getById(id) ?: return false
        val gamesRoot = context.filesDir.resolve("games").canonicalFile
        val ownedPath = context.filesDir.resolve(game.relativePath).canonicalFile
        require(ownedPath.toPath().startsWith(gamesRoot.toPath())) { "Game path escapes app-owned storage" }
        if (ownedPath.exists() && !ownedPath.deleteRecursively()) return false
        return gameDao.deleteById(id) > 0
    }
}

private fun GameEntity.toModel(): Game =
    Game(
        id = id,
        title = title,
        relativePath = relativePath,
        subtitle = subtitle,
        detail = detail,
        lastLaunchedAtMs = lastLaunchedAtMs,
    )
