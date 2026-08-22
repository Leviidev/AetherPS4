package com.bachatas4.android.data

import java.io.File

class InstallCleanup(
    private val filesDir: File,
    private val jobStore: InstallJobStore = InstallJobStore(filesDir),
) {
    fun cleanupJob(jobId: String) {
        val job = jobStore.read(jobId)
        job?.stagingDir?.let { rel ->
            val staging = File(filesDir, rel)
            if (staging.exists() && isStagingPath(staging)) {
                staging.deleteRecursively()
            }
        }
        job?.cachePath?.let { rel ->
            val cache = File(filesDir, rel)
            if (cache.exists() && isCachePath(cache)) {
                cache.delete()
            }
        }
        // Also try conventional paths if job record missing fields
        val stagingFallback = File(filesDir, "games/.import-$jobId")
        if (stagingFallback.exists() && isStagingPath(stagingFallback)) {
            stagingFallback.deleteRecursively()
        }
        val cacheFallback = File(filesDir, "pkg-cache/$jobId.pkg")
        if (cacheFallback.exists() && isCachePath(cacheFallback)) {
            cacheFallback.delete()
        }
        jobStore.delete(jobId)
    }

    fun cleanupStaleArtifacts(importBusy: Boolean) {
        if (importBusy) return
        val gamesRoot = File(filesDir, "games")
        if (gamesRoot.isDirectory) {
            gamesRoot.listFiles()
                ?.filter { it.isDirectory && it.name.startsWith(".import-") }
                ?.forEach { it.deleteRecursively() }
        }
        jobStore.list().forEach { job ->
            when (job.state) {
                InstallJob.STATE_INSTALLED,
                InstallJob.STATE_FAILED,
                -> cleanupJob(job.jobId)
                else -> {
                    // Leave resumable jobs for findResumableJob; drop ancient unknown
                    if (job.version != 1) cleanupJob(job.jobId)
                }
            }
        }
    }

    /**
     * Returns at most one job that is safe to resume after cleanup of terminal jobs.
     * Caller decides whether to restart work; this method does not start import.
     */
    fun findResumableJob(): InstallJob? {
        val resumableStates = setOf(
            InstallJob.STATE_EXTRACTING,
            InstallJob.STATE_COPYING,
            InstallJob.STATE_VERIFYING,
            InstallJob.STATE_REGISTERING,
            InstallJob.STATE_CHECKING_STORAGE,
            InstallJob.STATE_READING_METADATA,
            InstallJob.STATE_VALIDATING,
            InstallJob.STATE_SELECTED,
            InstallJob.STATE_NEED_PASSCODE,
            InstallJob.STATE_NEED_COPY_CONFIRM,
        )
        val candidates = jobStore.list().filter { it.state in resumableStates }
        if (candidates.isEmpty()) return null
        // Prefer the most recently updated job; clean the rest.
        val ordered = candidates.sortedByDescending { it.updatedAtMs }
        ordered.drop(1).forEach { cleanupJob(it.jobId) }
        return ordered.firstOrNull()
    }

    private fun isStagingPath(file: File): Boolean {
        val games = File(filesDir, "games").canonicalFile
        val path = runCatching { file.canonicalFile }.getOrNull() ?: return false
        return path.toPath().startsWith(games.toPath()) && path.name.startsWith(".import-")
    }

    private fun isCachePath(file: File): Boolean {
        val cacheRoot = File(filesDir, "pkg-cache").canonicalFile
        val path = runCatching { file.canonicalFile }.getOrNull() ?: return false
        return path.toPath().startsWith(cacheRoot.toPath())
    }
}
