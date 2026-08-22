package com.bachatas4.android.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

sealed interface ImportProgress {
    data object Idle : ImportProgress

    data class Selected(val sourceUri: String, val mode: String) : ImportProgress

    data class Validating(val sourceUri: String, val mode: String) : ImportProgress

    data class ReadingMetadata(
        val displayName: String,
        val contentId: String?,
    ) : ImportProgress

    data class CheckingStorage(
        val contentId: String,
        val requiredBytes: Long,
        val freeBytes: Long,
    ) : ImportProgress

    data class Extracting(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Copying(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Verifying(val title: String) : ImportProgress

    data class Registering(val title: String) : ImportProgress

    data class NeedPasscode(val contentId: String, val titleHint: String?) : ImportProgress

    /**
     * PKG import paused before local cache copy so the user can confirm
     * there is enough free storage for package + extract peak usage.
     */
    data class NeedCopyConfirm(
        val contentId: String,
        val titleHint: String?,
        val packageBytes: Long,
        val extractBytes: Long,
        val requiredBytes: Long,
        val freeBytes: Long,
    ) : ImportProgress

    data class Installed(val gameId: String, val title: String) : ImportProgress

    data class Failed(val code: InstallErrorCode, val message: String) : ImportProgress

    data class BatchSelected(
        val gameId: String,
        val gameTitle: String,
        val packageCount: Int,
    ) : ImportProgress

    data class BatchExtracting(
        val index: Int,
        val total: Int,
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class BatchInstalled(
        val gameId: String,
        val title: String,
        val count: Int,
    ) : ImportProgress

    data class BatchFailed(
        val code: InstallErrorCode,
        val message: String,
        val completedCount: Int,
        val totalCount: Int,
    ) : ImportProgress
}

object ImportManager {
    const val ACTION_IMPORT = "com.bachatas4.android.action.IMPORT_GAME"
    const val ACTION_CANCEL = "com.bachatas4.android.action.CANCEL_IMPORT"
    const val ACTION_SUBMIT_PASSCODE = "com.bachatas4.android.action.SUBMIT_PASSCODE"
    const val ACTION_CONFIRM_PKG_COPY = "com.bachatas4.android.action.CONFIRM_PKG_COPY"
    const val ACTION_IMPORT_PKGS = "com.bachatas4.android.action.IMPORT_PKGS"
    const val EXTRA_URI = "source_uri"
    const val EXTRA_MODE = "import_mode"
    const val EXTRA_PASSCODE = "passcode"
    const val EXTRA_GAME_ID = "game_id"
    const val EXTRA_URIS = "source_uris" // String ArrayList extra
    const val MODE_FOLDER = "folder"
    const val MODE_PKG = "pkg"
    const val MODE_PKG_BATCH = "pkg-batch"
    const val SERVICE_CLASS = "com.bachatas4.android.service.ImportService"

    private val _progress = MutableStateFlow<ImportProgress>(ImportProgress.Idle)
    val progress: StateFlow<ImportProgress> = _progress

    fun isBusy(state: ImportProgress = _progress.value): Boolean =
        when (state) {
            is ImportProgress.Idle,
            is ImportProgress.Installed,
            is ImportProgress.Failed,
            is ImportProgress.BatchInstalled,
            is ImportProgress.BatchFailed,
            -> false
            else -> true
        }

    /**
     * Atomically claim the single import slot and enter [ImportProgress.Selected].
     * Returns false when an import is already in progress.
     */
    fun tryBeginImport(sourceUri: String = "", mode: String = ""): Boolean {
        while (true) {
            val current = _progress.value
            if (isBusy(current)) return false
            if (_progress.compareAndSet(current, ImportProgress.Selected(sourceUri, mode))) {
                return true
            }
        }
    }

    fun update(state: ImportProgress) {
        _progress.value = state
    }

    fun reset() {
        _progress.value = ImportProgress.Idle
    }
}
