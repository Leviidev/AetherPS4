package com.bachatas4.android.runtime.process

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.util.Comparator
import java.util.UUID
import java.util.zip.ZipInputStream
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

data class InstalledCustomVulkanDriver(val name: String, val library: Path, val icdManifest: Path)

class CustomVulkanDriverInstaller(private val installRoot: Path) {
    fun install(input: InputStream): InstalledCustomVulkanDriver {
        val entries = readArchive(input)
        val metadataBytes = requireNotNull(entries[META_FILE]) { "Driver package is missing $META_FILE" }
        val metadata = runCatching { json.decodeFromString<Metadata>(metadataBytes.decodeToString()) }
            .getOrElse { throw IllegalArgumentException("Driver metadata is invalid", it) }
        require(metadata.schemaVersion == 1) { "Unsupported driver metadata schema" }
        require(metadata.name.isNotBlank()) { "Driver name is missing" }
        require(metadata.abi == GLIBC_ABI) { "Driver must use ARM64 glibc ABI" }
        require(metadata.libraryName.matches(LIBRARY_NAME)) { "Driver library name is invalid" }
        val libraryBytes = requireNotNull(entries[metadata.libraryName]) { "Driver package is missing ${metadata.libraryName}" }
        validateArm64GlibcElf(libraryBytes)

        Files.createDirectories(installRoot.parent)
        val staging = installRoot.resolveSibling("${installRoot.fileName}.staging-${UUID.randomUUID()}")
        deleteTree(staging)
        Files.createDirectories(staging)
        try {
            Files.write(staging.resolve(metadata.libraryName), libraryBytes)
            Files.write(staging.resolve(META_FILE), metadataBytes)
            val finalLibrary = installRoot.resolve(metadata.libraryName).toAbsolutePath()
            Files.write(staging.resolve(ICD_FILE), icdJson(finalLibrary).encodeToByteArray())
            deleteTree(installRoot)
            try {
                Files.move(staging, installRoot, StandardCopyOption.ATOMIC_MOVE)
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(staging, installRoot)
            }
        } finally {
            deleteTree(staging)
        }
        return requireNotNull(load()) { "Installed driver could not be loaded" }
    }

    fun load(): InstalledCustomVulkanDriver? {
        val metadataFile = installRoot.resolve(META_FILE)
        if (!Files.isRegularFile(metadataFile)) return null
        val metadata = runCatching {
            json.decodeFromString<Metadata>(Files.readAllBytes(metadataFile).decodeToString())
        }.getOrNull() ?: return null
        val library = installRoot.resolve(metadata.libraryName)
        val icd = installRoot.resolve(ICD_FILE)
        if (!Files.isRegularFile(library) || !Files.isRegularFile(icd)) return null
        return InstalledCustomVulkanDriver(metadata.name, library, icd)
    }

    private fun readArchive(input: InputStream): Map<String, ByteArray> {
        val entries = linkedMapOf<String, ByteArray>()
        var expandedBytes = 0L
        ZipInputStream(input.buffered()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                val name = entry.name
                require(!entry.isDirectory && name.isNotBlank() && '/' !in name && '\\' !in name && name != "." && name != "..") {
                    "Invalid driver archive entry: $name"
                }
                require(name !in entries) { "Duplicate driver archive entry: $name" }
                val output = ByteArrayOutputStream()
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                while (true) {
                    val count = zip.read(buffer)
                    if (count < 0) break
                    expandedBytes += count
                    require(expandedBytes <= MAX_EXPANDED_BYTES) { "Driver archive exceeds 64 MiB" }
                    output.write(buffer, 0, count)
                }
                entries[name] = output.toByteArray()
                zip.closeEntry()
            }
        }
        return entries
    }

    private fun validateArm64GlibcElf(bytes: ByteArray) {
        require(bytes.size >= 64 && bytes[0] == 0x7f.toByte() && bytes[1] == 'E'.code.toByte() &&
            bytes[2] == 'L'.code.toByte() && bytes[3] == 'F'.code.toByte()) { "Driver library is not ELF" }
        require(bytes[4] == 2.toByte() && bytes[5] == 1.toByte()) { "Driver must be 64-bit little-endian ELF" }
        val machine = (bytes[18].toInt() and 0xff) or ((bytes[19].toInt() and 0xff) shl 8)
        require(machine == 183) { "Driver must target ARM64" }
        require(bytes.containsSequence("libc.so.6".toByteArray())) { "Driver must be linked against glibc" }
    }

    private fun icdJson(library: Path): String = JsonObject(mapOf(
        "file_format_version" to JsonPrimitive("1.0.1"),
        "ICD" to JsonObject(mapOf(
            "library_path" to JsonPrimitive(library.toString()),
            "library_arch" to JsonPrimitive("64"),
            "api_version" to JsonPrimitive("1.4.0"),
        )),
    )).toString()

    private fun deleteTree(path: Path) {
        if (!Files.exists(path)) return
        Files.walk(path).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists) }
    }

    private fun ByteArray.containsSequence(needle: ByteArray): Boolean =
        indices.any { start -> start + needle.size <= size && needle.indices.all { this[start + it] == needle[it] } }

    @Serializable
    private data class Metadata(val schemaVersion: Int, val name: String, val libraryName: String, val abi: String? = null)

    private companion object {
        const val META_FILE = "meta.json"
        const val ICD_FILE = "freedreno_icd.aarch64.json"
        const val GLIBC_ABI = "linux-aarch64-glibc"
        const val MAX_EXPANDED_BYTES = 64L * 1024L * 1024L
        val LIBRARY_NAME = Regex("[A-Za-z0-9._-]+\\.so(?:\\.[0-9]+)*")
        val json = Json { ignoreUnknownKeys = true }
    }
}
