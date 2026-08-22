package com.bachatas4.android.data

import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

data class InstallManifest(
    val version: Int = 1,
    val status: String,
    val gameId: String,
    val contentId: String?,
    val mode: String,
    val sourceUri: String,
    val installedAtMs: Long,
    val requiredFiles: List<String>,
    val bytesTotal: Long,
    val overlays: List<OverlayRecord> = emptyList(),
)

data class OverlayRecord(
    val contentId: String?,
    val sourceUri: String,
    val appliedAtMs: Long,
)

object InstallManifestIo {
    const val FILE_NAME = "install.manifest"
    const val STATUS_INSTALLED = "INSTALLED"

    fun write(dir: File, manifest: InstallManifest) {
        dir.mkdirs()
        val target = File(dir, FILE_NAME)
        val tmp = File(dir, "$FILE_NAME.tmp")
        tmp.writeText(encode(manifest))
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

    fun read(dir: File): InstallManifest? {
        val file = File(dir, FILE_NAME)
        if (!file.isFile) return null
        return runCatching { decode(file.readText()) }.getOrNull()
    }

    internal fun encode(manifest: InstallManifest): String {
        val files = manifest.requiredFiles.joinToString(",") { escape(it) }
        return buildString {
            append("version=").append(manifest.version).append('\n')
            append("status=").append(escape(manifest.status)).append('\n')
            append("gameId=").append(escape(manifest.gameId)).append('\n')
            append("contentId=").append(escape(manifest.contentId.orEmpty())).append('\n')
            append("mode=").append(escape(manifest.mode)).append('\n')
            append("sourceUri=").append(escape(manifest.sourceUri)).append('\n')
            append("installedAtMs=").append(manifest.installedAtMs).append('\n')
            append("requiredFiles=").append(files).append('\n')
            append("bytesTotal=").append(manifest.bytesTotal).append('\n')
            val ov = manifest.overlays.joinToString(",") { o ->
                escape((o.contentId.orEmpty()) + "@" + o.appliedAtMs + "|" + o.sourceUri)
            }
            append("overlays=").append(ov).append('\n')
        }
    }

    internal fun decode(text: String): InstallManifest {
        val map = linkedMapOf<String, String>()
        text.lineSequence().forEach { line ->
            val idx = line.indexOf('=')
            if (idx <= 0) return@forEach
            map[line.substring(0, idx)] = unescape(line.substring(idx + 1))
        }
        val required = map["requiredFiles"]
            ?.split(',')
            ?.map { it.trim() }
            ?.filter { it.isNotEmpty() }
            ?: listOf("eboot.bin", "sce_sys/param.sfo")
        val overlays = map["overlays"]
            ?.split(',')
            ?.map { it.trim() }
            ?.filter { it.isNotEmpty() }
            ?.map { entry ->
                val at = entry.indexOf('@')
                val pipe = entry.indexOf('|', startIndex = (at + 1).coerceAtLeast(0))
                if (at < 0 || pipe < 0 || pipe <= at) {
                    OverlayRecord(null, unescape(entry), 0L)
                } else {
                    val cid = unescape(entry.substring(0, at)).takeIf { it.isNotBlank() }
                    val ts = unescape(entry.substring(at + 1, pipe)).toLongOrNull() ?: 0L
                    val uri = unescape(entry.substring(pipe + 1))
                    OverlayRecord(cid, uri, ts)
                }
            }
            ?: emptyList()
        return InstallManifest(
            version = map["version"]?.toIntOrNull() ?: 1,
            status = map["status"] ?: error("missing status"),
            gameId = map["gameId"] ?: error("missing gameId"),
            contentId = map["contentId"]?.takeIf { it.isNotBlank() },
            mode = map["mode"] ?: error("missing mode"),
            sourceUri = map["sourceUri"].orEmpty(),
            installedAtMs = map["installedAtMs"]?.toLongOrNull() ?: 0L,
            requiredFiles = required,
            bytesTotal = map["bytesTotal"]?.toLongOrNull() ?: 0L,
            overlays = overlays,
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
