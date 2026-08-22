package com.bachatas4.android.data

import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

data class InstallJob(
    val version: Int = 1,
    val jobId: String,
    val state: String,
    val mode: String,
    val sourceUri: String,
    val uriPersistable: Boolean = false,
    val displayName: String? = null,
    val contentId: String? = null,
    val titleId: String? = null,
    val stagingDir: String? = null,
    val cachePath: String? = null,
    val packageBytes: Long = 0L,
    val extractBytes: Long = 0L,
    val requiredBytes: Long = 0L,
    val createdAtMs: Long,
    val updatedAtMs: Long,
    val lastErrorCode: String? = null,
    val lastErrorMessage: String? = null,
) {
    companion object {
        const val STATE_SELECTED = "SELECTED"
        const val STATE_VALIDATING = "VALIDATING"
        const val STATE_READING_METADATA = "READING_METADATA"
        const val STATE_CHECKING_STORAGE = "CHECKING_STORAGE"
        const val STATE_EXTRACTING = "EXTRACTING"
        const val STATE_COPYING = "COPYING"
        const val STATE_VERIFYING = "VERIFYING"
        const val STATE_REGISTERING = "REGISTERING"
        const val STATE_INSTALLED = "INSTALLED"
        const val STATE_FAILED = "FAILED"
        const val STATE_NEED_PASSCODE = "NEED_PASSCODE"
        const val STATE_NEED_COPY_CONFIRM = "NEED_COPY_CONFIRM"
    }
}

class InstallJobStore(private val filesDir: File) {
    fun jobsRoot(): File = File(filesDir, "games/.jobs")

    fun create(job: InstallJob) {
        write(job)
    }

    fun update(job: InstallJob) {
        write(job)
    }

    fun read(jobId: String): InstallJob? {
        val file = File(jobDir(jobId), "job.json")
        if (!file.isFile) return null
        return runCatching { decode(file.readText()) }.getOrNull()
    }

    fun list(): List<InstallJob> {
        val root = jobsRoot()
        if (!root.isDirectory) return emptyList()
        return root.listFiles()
            ?.filter { it.isDirectory }
            ?.mapNotNull { read(it.name) }
            .orEmpty()
    }

    fun delete(jobId: String) {
        val dir = jobDir(jobId)
        if (dir.exists()) {
            dir.deleteRecursively()
        }
    }

    private fun jobDir(jobId: String): File = File(jobsRoot(), jobId)

    private fun write(job: InstallJob) {
        val dir = jobDir(job.jobId)
        dir.mkdirs()
        val target = File(dir, "job.json")
        val tmp = File(dir, "job.json.tmp")
        tmp.writeText(encode(job))
        try {
            Files.move(
                tmp.toPath(),
                target.toPath(),
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
        } catch (_: AtomicMoveNotSupportedException) {
            if (target.exists()) target.delete()
            if (!tmp.renameTo(target)) {
                tmp.copyTo(target, overwrite = true)
                tmp.delete()
            }
        }
    }

    internal fun encode(job: InstallJob): String =
        buildString {
            fun put(key: String, value: String?) {
                append(key).append('=').append(escape(value.orEmpty())).append('\n')
            }
            put("version", job.version.toString())
            put("jobId", job.jobId)
            put("state", job.state)
            put("mode", job.mode)
            put("sourceUri", job.sourceUri)
            put("uriPersistable", job.uriPersistable.toString())
            put("displayName", job.displayName)
            put("contentId", job.contentId)
            put("titleId", job.titleId)
            put("stagingDir", job.stagingDir)
            put("cachePath", job.cachePath)
            put("packageBytes", job.packageBytes.toString())
            put("extractBytes", job.extractBytes.toString())
            put("requiredBytes", job.requiredBytes.toString())
            put("createdAtMs", job.createdAtMs.toString())
            put("updatedAtMs", job.updatedAtMs.toString())
            put("lastErrorCode", job.lastErrorCode)
            put("lastErrorMessage", job.lastErrorMessage)
        }

    private fun decode(text: String): InstallJob {
        val map = linkedMapOf<String, String>()
        text.lineSequence().forEach { line ->
            val idx = line.indexOf('=')
            if (idx <= 0) return@forEach
            map[line.substring(0, idx)] = unescape(line.substring(idx + 1))
        }
        fun opt(key: String): String? = map[key]?.takeIf { it.isNotBlank() }
        return InstallJob(
            version = map["version"]?.toIntOrNull() ?: 1,
            jobId = map["jobId"] ?: error("missing jobId"),
            state = map["state"] ?: error("missing state"),
            mode = map["mode"] ?: error("missing mode"),
            sourceUri = map["sourceUri"] ?: "",
            uriPersistable = map["uriPersistable"]?.toBooleanStrictOrNull() ?: false,
            displayName = opt("displayName"),
            contentId = opt("contentId"),
            titleId = opt("titleId"),
            stagingDir = opt("stagingDir"),
            cachePath = opt("cachePath"),
            packageBytes = map["packageBytes"]?.toLongOrNull() ?: 0L,
            extractBytes = map["extractBytes"]?.toLongOrNull() ?: 0L,
            requiredBytes = map["requiredBytes"]?.toLongOrNull() ?: 0L,
            createdAtMs = map["createdAtMs"]?.toLongOrNull() ?: 0L,
            updatedAtMs = map["updatedAtMs"]?.toLongOrNull() ?: 0L,
            lastErrorCode = opt("lastErrorCode"),
            lastErrorMessage = opt("lastErrorMessage"),
        )
    }

    private fun escape(value: String): String =
        value.replace("\\", "\\\\").replace("\n", "\\n").replace("=", "\\=")

    private fun unescape(value: String): String {
        val out = StringBuilder()
        var i = 0
        while (i < value.length) {
            val c = value[i]
            if (c == '\\' && i + 1 < value.length) {
                when (value[i + 1]) {
                    'n' -> out.append('\n')
                    '\\' -> out.append('\\')
                    '=' -> out.append('=')
                    else -> out.append(value[i + 1])
                }
                i += 2
            } else {
                out.append(c)
                i++
            }
        }
        return out.toString()
    }
}
