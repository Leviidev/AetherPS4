package com.bachatas4.android.runtime.driver

import java.nio.file.Files
import java.nio.file.Path
import java.util.Comparator
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.json.Json

class DriverRegistry(private val root: Path) {
    private val state = MutableStateFlow<List<InstalledDriver>>(emptyList())

    fun observeInstalled(): StateFlow<List<InstalledDriver>> {
        refresh()
        return state
    }

    fun listInstalled(): List<InstalledDriver> = refresh()

    fun resolve(id: String): InstalledDriver? {
        require(ID_PATTERN.matches(id)) { "Invalid driver id: $id" }
        return load(root.resolve(id))
    }

    fun remove(id: String): Boolean {
        require(ID_PATTERN.matches(id)) { "Invalid driver id: $id" }
        val directory = root.resolve(id).normalize()
        require(directory.parent == root.normalize()) { "Driver path escapes registry" }
        if (!Files.exists(directory)) return false
        deleteTree(directory)
        refresh()
        return true
    }

    private fun refresh(): List<InstalledDriver> {
        val installed = if (!Files.isDirectory(root)) {
            emptyList()
        } else {
            Files.list(root).use { paths ->
                paths.iterator().asSequence()
                    .filter { Files.isDirectory(it) && ID_PATTERN.matches(it.fileName.toString()) }
                    .map(::load)
                    .filter { it != null }
                    .map { requireNotNull(it) }
                    .sortedBy { it.metadata.displayName.lowercase() }
                    .toList()
            }
        }
        state.value = installed
        return installed
    }

    private fun load(directory: Path): InstalledDriver? {
        val metadataFile = directory.resolve(METADATA_FILE)
        if (!Files.isRegularFile(metadataFile)) return null
        val metadata = runCatching {
            json.decodeFromString<InstalledDriverMetadata>(Files.readAllBytes(metadataFile).decodeToString())
        }.getOrNull() ?: return null
        if (metadata.schemaVersion != 1 || metadata.id != directory.fileName.toString()) return null
        if (!metadata.sha256.matches(Regex("[0-9a-f]{64}"))) return null
        val installed = InstalledDriver(metadata, directory)
        if (!Files.isRegularFile(installed.library)) return null
        if (metadata.icdRelativePath != null && !Files.isRegularFile(installed.icdManifest)) return null
        if (installed.library.parent != directory) return null
        if (installed.icdManifest?.parent?.let { it != directory } == true) return null
        return installed
    }

    private fun deleteTree(path: Path) {
        Files.walk(path).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists) }
    }

    companion object {
        const val METADATA_FILE = "installed-driver.json"
        val ID_PATTERN = Regex("turnip-[0-9a-f]{16}")
        private val json = Json { ignoreUnknownKeys = false }
    }
}
