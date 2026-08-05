package com.bachatas4.android.data

import java.io.File

object GameInstallVerifier {
    val REQUIRED_FILES: List<String> = listOf("eboot.bin", "sce_sys/param.sfo")

    sealed class VerifyResult {
        data class Ok(val bytesTotal: Long) : VerifyResult()
        data class Fail(val code: InstallErrorCode, val message: String) : VerifyResult()
    }

    fun requiredFilesPresent(gameDir: File): Boolean {
        val eboot = File(gameDir, "eboot.bin")
        val sfo = File(gameDir, "sce_sys/param.sfo")
        return eboot.isFile && eboot.length() > 0L && sfo.isFile && sfo.length() > 0L
    }

    fun canLaunch(filesDir: File, relativePath: String): Boolean {
        val gamesRoot = File(filesDir, "games").canonicalFile
        val root = File(filesDir, relativePath).canonicalFile
        if (!root.toPath().startsWith(gamesRoot.toPath())) return false
        if (!root.isDirectory) return false
        val manifest = InstallManifestIo.read(root) ?: return false
        if (manifest.status != InstallManifestIo.STATUS_INSTALLED) return false
        val eboot = File(root, "eboot.bin")
        return eboot.isFile && eboot.length() > 0L
    }

    fun verifyTreeForRegistration(
        gameDir: File,
        expectedGameId: String?,
    ): VerifyResult {
        if (!gameDir.isDirectory) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "Game directory missing")
        }
        val eboot = File(gameDir, "eboot.bin")
        if (!eboot.isFile || eboot.length() <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "missing or empty eboot.bin")
        }
        val sfoFile = File(gameDir, "sce_sys/param.sfo")
        if (!sfoFile.isFile || sfoFile.length() <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "missing sce_sys/param.sfo")
        }
        if (expectedGameId != null) {
            val meta = runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
            val titleId = meta?.titleId
            if (!titleId.isNullOrBlank() &&
                !titleId.equals(expectedGameId, ignoreCase = true) &&
                !expectedGameId.startsWith(titleId, ignoreCase = true)
            ) {
                // Folder imports often use folder name as id; only hard-fail on clear mismatch
                // when both look like TITLE_ID form and differ.
                if (TITLE_ID.matches(expectedGameId) && TITLE_ID.matches(titleId) &&
                    !expectedGameId.equals(titleId, ignoreCase = true)
                ) {
                    return VerifyResult.Fail(
                        InstallErrorCode.VERIFY_FAILED,
                        "title id mismatch: expected $expectedGameId got $titleId",
                    )
                }
            }
        }
        var total = 0L
        gameDir.walkTopDown().forEach { file ->
            if (file.isFile) total += file.length().coerceAtLeast(0L)
        }
        if (total <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "empty game tree")
        }
        return VerifyResult.Ok(total)
    }

    private val TITLE_ID = Regex("^[A-Za-z]{4}[0-9]{5}$")
}
