package com.bachatas4.android.runtime.install

import java.io.InputStream
import java.security.MessageDigest
import kotlinx.serialization.Serializable

@Serializable
data class RuntimeManifest(
    val schemaVersion: Int,
    val runtimeVersion: String,
    val protocolVersion: Int,
    val files: List<RuntimeFile>,
)

@Serializable
data class RuntimeFile(
    val path: String,
    val size: Long,
    val sha256: String,
)

fun sha256(input: InputStream): String {
    val digest = MessageDigest.getInstance("SHA-256")
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    while (true) {
        val count = input.read(buffer)
        if (count < 0) break
        digest.update(buffer, 0, count)
    }
    return digest.digest().toHexString()
}

internal fun ByteArray.toHexString(): String = joinToString(separator = "") { byte ->
    "%02x".format(byte.toInt() and 0xff)
}
