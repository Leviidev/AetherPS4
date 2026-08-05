package com.bachatas4.android.data

import java.io.File

data class LibraryInsert(
    val id: String,
    val title: String,
    val relativePath: String,
    val subtitle: String? = null,
    val detail: String? = null,
    val sourceUri: String = "",
)

data class LibrarySyncPlan(
    val inserts: List<LibraryInsert>,
    val stagingDirsToDelete: List<File>,
    val healManifestDirs: List<File>,
    val dbIdsToDrop: List<String>,
)

/**
 * Pure planner for library folder reconciliation. Does not touch the database.
 */
object LibrarySync {
    fun planSync(
        gamesRoot: File,
        dbIds: Set<String>,
        importBusy: Boolean,
        filesDir: File = gamesRoot.parentFile ?: gamesRoot,
    ): LibrarySyncPlan {
        if (!gamesRoot.isDirectory) {
            return LibrarySyncPlan(emptyList(), emptyList(), emptyList(), emptyList())
        }
        val inserts = ArrayList<LibraryInsert>()
        val staging = ArrayList<File>()
        val heal = ArrayList<File>()
        val drop = ArrayList<String>()

        val folders = gamesRoot.listFiles()?.filter { it.isDirectory }.orEmpty()
        for (folder in folders) {
            val name = folder.name
            if (name == ".jobs") continue
            if (name.startsWith(".import-")) {
                if (!importBusy) staging.add(folder)
                continue
            }
            if (name.startsWith(".")) continue

            val verify = GameInstallVerifier.verifyTreeForRegistration(folder, name)
            if (verify is GameInstallVerifier.VerifyResult.Fail) {
                if (name in dbIds) drop.add(name)
                continue
            }

            val manifest = InstallManifestIo.read(folder)
            if (manifest == null || manifest.status != InstallManifestIo.STATUS_INSTALLED) {
                heal.add(folder)
            }

            if (name !in dbIds) {
                val sfoFile = File(folder, "sce_sys/param.sfo")
                val sfo = if (sfoFile.isFile) {
                    runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
                } else {
                    null
                }
                val resolved = GameMetadataResolver.resolve(folderName = name, sfo = sfo)
                inserts.add(
                    LibraryInsert(
                        id = resolved.id,
                        title = resolved.title,
                        relativePath = "games/$name",
                        subtitle = resolved.subtitle,
                        detail = resolved.detail,
                        sourceUri = InstallManifestIo.read(folder)?.sourceUri.orEmpty(),
                    ),
                )
            } else if (!GameInstallVerifier.canLaunch(filesDir, "games/$name")) {
                // Has DB row but cannot launch — if tree verifies, heal manifest only
                if (manifest == null) heal.add(folder)
            }
        }

        // Drop DB ids whose folders vanished
        for (id in dbIds) {
            val dir = File(gamesRoot, id)
            if (!dir.isDirectory) drop.add(id)
        }

        return LibrarySyncPlan(
            inserts = inserts.distinctBy { it.id },
            stagingDirsToDelete = staging,
            healManifestDirs = heal.distinct(),
            dbIdsToDrop = drop.distinct(),
        )
    }

    fun applyHeals(plan: LibrarySyncPlan) {
        for (dir in plan.healManifestDirs) {
            val verify = GameInstallVerifier.verifyTreeForRegistration(dir, dir.name)
            if (verify !is GameInstallVerifier.VerifyResult.Ok) continue
            val existing = InstallManifestIo.read(dir)
            InstallManifestIo.write(
                dir,
                InstallManifest(
                    status = InstallManifestIo.STATUS_INSTALLED,
                    gameId = dir.name,
                    contentId = existing?.contentId,
                    mode = existing?.mode ?: ImportManager.MODE_FOLDER,
                    sourceUri = existing?.sourceUri.orEmpty(),
                    installedAtMs = existing?.installedAtMs ?: System.currentTimeMillis(),
                    requiredFiles = GameInstallVerifier.REQUIRED_FILES,
                    bytesTotal = verify.bytesTotal,
                ),
            )
        }
        for (staging in plan.stagingDirsToDelete) {
            staging.deleteRecursively()
        }
    }
}
