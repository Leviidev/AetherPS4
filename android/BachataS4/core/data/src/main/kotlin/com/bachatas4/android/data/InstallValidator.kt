package com.bachatas4.android.data

import java.io.File

object InstallValidator {
    fun checkStorage(requiredBytes: Long, freeBytes: Long): InstallErrorCode? =
        if (freeBytes < requiredBytes) InstallErrorCode.INSUFFICIENT_STORAGE else null

    /**
     * @param destExists whether games/<id> exists
     * @param destLaunchable whether existing dest passes canLaunch
     * @param destPartial incomplete tree without valid install
     */
    fun checkDestination(
        destExists: Boolean,
        destLaunchable: Boolean,
        destPartial: Boolean,
    ): InstallErrorCode? =
        when {
            destExists && destLaunchable -> InstallErrorCode.ALREADY_INSTALLED
            destExists && destPartial -> InstallErrorCode.PARTIAL_EXISTS
            else -> null
        }

    fun checkWritableGamesDir(filesDir: File): InstallErrorCode? {
        val games = File(filesDir, "games")
        return try {
            games.mkdirs()
            if (!games.isDirectory || !games.canWrite()) {
                InstallErrorCode.DEST_NOT_WRITABLE
            } else {
                null
            }
        } catch (_: SecurityException) {
            InstallErrorCode.DEST_NOT_WRITABLE
        }
    }

    fun mapProbeError(message: String?): InstallErrorCode {
        val m = message.orEmpty().lowercase()
        return when {
            "encrypt" in m || "key" in m && "pass" !in m -> InstallErrorCode.UNSUPPORTED_ENCRYPTION
            "header" in m || "magic" in m -> InstallErrorCode.BAD_HEADER
            "malform" in m || "invalid" in m || "corrupt" in m -> InstallErrorCode.MALFORMED_PACKAGE
            else -> InstallErrorCode.MALFORMED_PACKAGE
        }
    }
}
