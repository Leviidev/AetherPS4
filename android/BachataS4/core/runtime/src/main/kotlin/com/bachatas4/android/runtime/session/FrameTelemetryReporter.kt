package com.bachatas4.android.runtime.session

import java.util.Locale

data class FrameTelemetrySample(
    val elapsedMillis: Long,
    val fps: Float,
    val frameTimeMs: Float,
) {
    fun logLine(): String = String.format(
        Locale.ROOT,
        "elapsedMs=%d fps=%.2f frameTimeMs=%.2f",
        elapsedMillis,
        fps,
        frameTimeMs,
    )
}

class FrameTelemetryReporter(
    private val intervalNanos: Long = 2_000_000_000L,
) {
    private var startedAtNanos: Long? = null
    private var lastReportedAtNanos: Long? = null

    init {
        require(intervalNanos > 0) { "Telemetry interval must be positive" }
    }

    fun record(nowNanos: Long, telemetry: FrameTelemetry): FrameTelemetrySample? {
        require(nowNanos >= 0) { "Telemetry timestamp must not be negative" }
        val startedAt = startedAtNanos
        if (startedAt == null) {
            startedAtNanos = nowNanos
            lastReportedAtNanos = nowNanos
            return FrameTelemetrySample(0L, telemetry.fps, telemetry.frameTimeMs)
        }
        val lastReportedAt = requireNotNull(lastReportedAtNanos)
        require(nowNanos >= lastReportedAt) { "Telemetry timestamp moved backwards" }
        if (nowNanos - lastReportedAt < intervalNanos) return null
        lastReportedAtNanos = nowNanos
        return FrameTelemetrySample(
            elapsedMillis = (nowNanos - startedAt) / 1_000_000L,
            fps = telemetry.fps,
            frameTimeMs = telemetry.frameTimeMs,
        )
    }

    fun reset() {
        startedAtNanos = null
        lastReportedAtNanos = null
    }
}
