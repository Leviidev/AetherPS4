package com.bachatas4.android.data

import com.bachatas4.android.runtime.settings.ProfileScope
import java.io.File

fun interface LegacyDriverSettings { fun selectedDriver(): String? }

class LegacyRuntimeSettingsMigration(
    private val filesDir: File,
    private val profiles: RuntimeProfileStore,
    private val legacy: LegacyDriverSettings,
    private val validatedCustomDriverId: () -> String? = { null },
) {
    suspend fun migrate(): Boolean {
        val marker = File(filesDir, "settings/migrations/runtime-v1.complete")
        if (marker.isFile) return false
        val global = profiles.load(ProfileScope.Global)
        val backup = File(filesDir, "settings/backups/global-before-legacy-migration.json")
        if (!backup.isFile) {
            backup.parentFile?.mkdirs()
            backup.writeText(profiles.export(ProfileScope.Global))
        }
        val migratedId = when (legacy.selectedDriver()) {
            null, "SYSTEM" -> global.driverId
            "CUSTOM" -> validatedCustomDriverId()
            "TURNIP_25_0_0", "TURNIP_25_3_0_R11", "TURNIP_26_1_0" -> null
            else -> null
        }
        profiles.update(ProfileScope.Global) { it.copy(driverId = migratedId) }
        check(profiles.load(ProfileScope.Global).driverId == migratedId) { "Migrated profile verification failed" }
        marker.parentFile?.mkdirs()
        val temporary = File(marker.parentFile, "${marker.name}.tmp")
        temporary.writeText("complete\n")
        check(temporary.renameTo(marker) || run { temporary.copyTo(marker, overwrite = true); temporary.delete(); true })
        return true
    }
}
