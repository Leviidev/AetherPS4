package com.bachatas4.android.runtime.driver

import java.nio.file.Path
import kotlinx.serialization.Serializable

@Serializable
enum class DriverAbi {
    ANDROID_BIONIC,
    LINUX_GLIBC,
}

@Serializable
data class DriverPackageSource(
    val repository: String? = null,
    val releaseTag: String? = null,
    val assetName: String,
)

@Serializable
data class InstalledDriverMetadata(
    val schemaVersion: Int = 1,
    val id: String,
    val displayName: String,
    val sourceRepository: String? = null,
    val releaseTag: String? = null,
    val assetName: String,
    val sha256: String,
    val installedAtMs: Long,
    val abi: DriverAbi,
    val minApi: Int? = null,
    val libraryRelativePath: String,
    val icdRelativePath: String? = null,
) {
    fun source(): DriverPackageSource = DriverPackageSource(sourceRepository, releaseTag, assetName)
}

data class InstalledDriver(
    val metadata: InstalledDriverMetadata,
    val root: Path,
) {
    val library: Path get() = root.resolve(metadata.libraryRelativePath)
    val icdManifest: Path? get() = metadata.icdRelativePath?.let(root::resolve)
}
