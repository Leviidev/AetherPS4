package com.bachatas4.android.runtime.driver

import android.os.Build
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.Comparator
import java.util.UUID
import java.util.zip.ZipInputStream
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

class TurnipPackageInstaller(
    private val registryRoot: Path,
    private val deviceApi: Int = Build.VERSION.SDK_INT,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    fun install(input: InputStream, source: DriverPackageSource): InstalledDriver {
        require(source.assetName.isNotBlank()) { "Driver asset name is blank" }
        val archive = readLimited(input, MAX_ARCHIVE_BYTES, "Driver archive exceeds 32 MiB")
        val hash = sha256(archive)
        val id = "turnip-${hash.take(16)}"
        val registry = DriverRegistry(registryRoot)
        registry.resolve(id)?.let { return it }

        val entries = readArchive(archive)
        val metadataEntries = entries.filterKeys { it.substringAfterLast('/') == PACKAGE_METADATA_FILE }
        require(metadataEntries.size == 1) { "Driver package must contain exactly one $PACKAGE_METADATA_FILE" }
        val (metadataPath, metadataBytes) = metadataEntries.entries.single()
        val metadata = runCatching { json.decodeFromString<PackageMetadata>(metadataBytes.decodeToString()) }
            .getOrElse { throw IllegalArgumentException("Driver metadata is invalid", it) }
        require(metadata.schemaVersion == 1) { "Unsupported driver metadata schema" }
        require(metadata.name.isNotBlank()) { "Driver name is blank" }
        require(metadata.libraryName.matches(LIBRARY_NAME)) { "Driver library name is invalid" }
        metadata.minApi?.let { require(deviceApi >= it) { "Driver requires Android API $it" } }

        val metadataDirectory = metadataPath.substringBeforeLast('/', "")
        val expectedLibrary = if (metadataDirectory.isEmpty()) metadata.libraryName else "$metadataDirectory/${metadata.libraryName}"
        val libraryBytes = entries[expectedLibrary] ?: entries.entries
            .singleOrNull { it.key.substringAfterLast('/') == metadata.libraryName }?.value
        requireNotNull(libraryBytes) { "Driver package is missing ${metadata.libraryName}" }
        val abi = validateArm64Elf(libraryBytes)

        Files.createDirectories(registryRoot)
        val finalRoot = registryRoot.resolve(id)
        val staging = registryRoot.resolve(".staging-${UUID.randomUUID()}")
        deleteTree(staging)
        Files.createDirectories(staging)
        val finalLibrary = finalRoot.resolve(metadata.libraryName).toAbsolutePath()
        val icdName = if (abi == DriverAbi.LINUX_GLIBC) ICD_FILE else null
        val installedMetadata = InstalledDriverMetadata(
            id = id,
            displayName = metadata.name,
            sourceRepository = source.repository,
            releaseTag = source.releaseTag,
            assetName = source.assetName,
            sha256 = hash,
            installedAtMs = clock(),
            abi = abi,
            minApi = metadata.minApi,
            libraryRelativePath = metadata.libraryName,
            icdRelativePath = icdName,
        )
        try {
            Files.write(staging.resolve(metadata.libraryName), libraryBytes)
            Files.write(staging.resolve(DriverRegistry.METADATA_FILE), json.encodeToString(installedMetadata).encodeToByteArray())
            if (icdName != null) Files.write(staging.resolve(icdName), icdJson(finalLibrary).encodeToByteArray())
            try {
                Files.move(staging, finalRoot, StandardCopyOption.ATOMIC_MOVE)
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(staging, finalRoot)
            }
        } catch (error: java.nio.file.FileAlreadyExistsException) {
            registry.resolve(id)?.let { return it }
            throw error
        } finally {
            deleteTree(staging)
        }
        return requireNotNull(registry.resolve(id)) { "Installed driver could not be loaded" }
    }

    private fun readArchive(archive: ByteArray): Map<String, ByteArray> {
        val entries = linkedMapOf<String, ByteArray>()
        var expanded = 0L
        var count = 0
        ZipInputStream(ByteArrayInputStream(archive)).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                count += 1
                require(count <= MAX_ENTRIES) { "Driver archive has too many entries" }
                val name = normalizeEntry(entry.name)
                if (entry.isDirectory) continue
                require(name.substringAfterLast('.').lowercase() !in NESTED_ARCHIVES) {
                    "Nested archives are not allowed: $name"
                }
                require(name !in entries) { "Duplicate driver archive entry: $name" }
                val output = ByteArrayOutputStream()
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                while (true) {
                    val read = zip.read(buffer)
                    if (read < 0) break
                    expanded += read
                    require(expanded <= MAX_EXPANDED_BYTES) { "Driver archive exceeds 64 MiB expanded" }
                    output.write(buffer, 0, read)
                }
                entries[name] = output.toByteArray()
                zip.closeEntry()
            }
        }
        return entries
    }

    private fun normalizeEntry(name: String): String {
        require(name.isNotBlank() && !name.startsWith('/') && !name.startsWith('\\')) {
            "Invalid driver archive entry: $name"
        }
        val parts = name.replace('\\', '/').split('/')
        require(parts.none { it.isBlank() || it == "." || it == ".." }) { "Invalid driver archive entry: $name" }
        return parts.joinToString("/")
    }

    private fun validateArm64Elf(bytes: ByteArray): DriverAbi {
        require(bytes.size >= 64 && bytes[0] == 0x7f.toByte() && bytes[1] == 'E'.code.toByte() &&
            bytes[2] == 'L'.code.toByte() && bytes[3] == 'F'.code.toByte()) { "Driver library is not ELF" }
        require(bytes[4] == 2.toByte() && bytes[5] == 1.toByte()) { "Driver must be 64-bit little-endian ELF" }
        val machine = (bytes[18].toInt() and 0xff) or ((bytes[19].toInt() and 0xff) shl 8)
        require(machine == 183) { "Driver must target ARM64" }
        return when {
            bytes.containsSequence("libc.so.6".toByteArray()) -> DriverAbi.LINUX_GLIBC
            bytes.containsSequence("libc.so".toByteArray()) -> DriverAbi.ANDROID_BIONIC
            else -> throw IllegalArgumentException("Driver C library ABI is unsupported")
        }
    }

    private fun readLimited(input: InputStream, maximum: Long, message: String): ByteArray {
        val output = ByteArrayOutputStream()
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var total = 0L
        input.use {
            while (true) {
                val read = it.read(buffer)
                if (read < 0) break
                total += read
                require(total <= maximum) { message }
                output.write(buffer, 0, read)
            }
        }
        return output.toByteArray()
    }

    private fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it) }

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
    private data class PackageMetadata(
        val schemaVersion: Int,
        val name: String,
        val libraryName: String,
        val packageVersion: String? = null,
        val minApi: Int? = null,
    )

    private companion object {
        const val PACKAGE_METADATA_FILE = "meta.json"
        const val ICD_FILE = "freedreno_icd.aarch64.json"
        const val MAX_ARCHIVE_BYTES = 32L * 1024L * 1024L
        const val MAX_EXPANDED_BYTES = 64L * 1024L * 1024L
        const val MAX_ENTRIES = 32
        val NESTED_ARCHIVES = setOf("zip", "7z", "rar", "tar", "gz", "xz", "zst", "tzst")
        val LIBRARY_NAME = Regex("[A-Za-z0-9._-]+\\.so(?:\\.[0-9]+)*")
        val json = Json { ignoreUnknownKeys = true }
    }
}
