package com.bachatas4.android.data

import com.bachatas4.android.runtime.settings.CURRENT_SCHEMA_VERSION
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeProfile
import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

class RuntimeProfileStore(private val filesDir: File) {
    private val json = Json {
        prettyPrint = true
        encodeDefaults = true
        ignoreUnknownKeys = true
    }
    private val mutex = Mutex()
    private val observed = ConcurrentHashMap<ProfileScope, MutableStateFlow<RuntimeProfile>>()

    suspend fun load(scope: ProfileScope): RuntimeProfile = mutex.withLock { loadUnlocked(scope) }

    fun observe(scope: ProfileScope): Flow<RuntimeProfile> =
        observed.getOrPut(scope) { MutableStateFlow(loadUnlocked(scope)) }

    suspend fun update(
        scope: ProfileScope,
        mutation: (RuntimeProfile) -> RuntimeProfile,
    ): RuntimeProfile = mutex.withLock {
        val updated = migrate(mutation(loadUnlocked(scope)))
        writeUnlocked(scope, updated)
        updated
    }

    suspend fun export(scope: ProfileScope): String = json.encodeToString(load(scope))

    suspend fun import(scope: ProfileScope, text: String): RuntimeProfile = mutex.withLock {
        val parsed = migrate(json.decodeFromString<RuntimeProfile>(text))
        writeUnlocked(scope, parsed)
        parsed
    }

    private fun loadUnlocked(scope: ProfileScope): RuntimeProfile {
        val file = profileFile(scope)
        if (!file.isFile) return RuntimeProfile()
        return migrate(json.decodeFromString<RuntimeProfile>(file.readText()))
    }

    private fun migrate(profile: RuntimeProfile): RuntimeProfile {
        require(profile.schemaVersion >= 0) { "Invalid runtime profile schema ${profile.schemaVersion}" }
        require(profile.schemaVersion <= CURRENT_SCHEMA_VERSION) {
            "Runtime profile schema ${profile.schemaVersion} is newer than supported $CURRENT_SCHEMA_VERSION"
        }
        return if (profile.schemaVersion == CURRENT_SCHEMA_VERSION) profile else {
            profile.copy(schemaVersion = CURRENT_SCHEMA_VERSION)
        }
    }

    private fun writeUnlocked(scope: ProfileScope, profile: RuntimeProfile) {
        val destination = profileFile(scope)
        val parent = requireNotNull(destination.parentFile) { "Profile path has no parent: $destination" }
        parent.mkdirs()
        val temporary = File(parent, "${destination.name}.tmp")
        val backup = File(parent, "${destination.name}.bak")
        FileOutputStream(temporary).use { output ->
            output.write(json.encodeToString(profile).toByteArray(Charsets.UTF_8))
            output.fd.sync()
        }
        if (destination.isFile) {
            Files.copy(destination.toPath(), backup.toPath(), StandardCopyOption.REPLACE_EXISTING)
        }
        try {
            Files.move(
                temporary.toPath(),
                destination.toPath(),
                StandardCopyOption.REPLACE_EXISTING,
                StandardCopyOption.ATOMIC_MOVE,
            )
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(temporary.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING)
        } finally {
            temporary.delete()
        }
        observed[scope]?.value = profile
    }

    private fun profileFile(scope: ProfileScope): File = when (scope) {
        ProfileScope.Global -> File(filesDir, "settings/global.json")
        is ProfileScope.Game -> File(filesDir, "settings/games/${scope.gameId}.json")
    }
}
