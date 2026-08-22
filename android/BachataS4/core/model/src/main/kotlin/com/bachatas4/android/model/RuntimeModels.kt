package com.bachatas4.android.model

import kotlinx.serialization.Serializable

@Serializable
enum class RuntimeErrorCode {
    UNSUPPORTED_DEVICE,
    RUNTIME_MISSING,
    RUNTIME_CORRUPT,
    INSUFFICIENT_STORAGE,
    CONTENT_PERMISSION_LOST,
    CONTENT_INVALID,
    VULKAN_UNSUPPORTED,
    DRIVER_BLOCKED,
    TRANSLATOR_START_FAILED,
    BACKEND_CRASHED,
    PROTOCOL_MISMATCH,
}

@Serializable
sealed interface RuntimeState {
    @Serializable
    data object Idle : RuntimeState

    @Serializable
    data class Preparing(val stage: String) : RuntimeState

    @Serializable
    data class Running(val sessionId: String) : RuntimeState

    @Serializable
    data object Stopping : RuntimeState

    @Serializable
    data class Stopped(val exitCode: Int) : RuntimeState

    @Serializable
    data class Failed(
        val code: RuntimeErrorCode,
        val detail: String,
    ) : RuntimeState
}

@Serializable
data class Game(
    val id: String,
    val title: String,
    val relativePath: String,
    val subtitle: String? = null,
    val detail: String? = null,
    val lastLaunchedAtMs: Long = 0L,
)

@Serializable
data class DeviceProfile(
    val soc: String,
    val gpu: String,
    val supported: Boolean,
)

@Serializable
data class LaunchRequest(
    val gameId: String,
    val ebootPath: String,
)

@Serializable
data class DiagnosticEvent(
    val timestampMs: Long,
    val category: String,
    val message: String,
)
