package com.bachatas4.android.runtime.session

import org.junit.Assert.assertEquals
import org.junit.Test

class ManagedSessionTelemetryTest {
    @Test
    fun presentedFramesProduceFpsAndFrameTime() {
        ManagedSession.recordPresentedFrame(1_000_000_000L)
        ManagedSession.recordPresentedFrame(1_033_333_333L)

        val telemetry = ManagedSession.frameTelemetry.value
        assertEquals(30f, telemetry.fps, 0.1f)
        assertEquals(33.33f, telemetry.frameTimeMs, 0.1f)
    }

    @Test
    fun staleFramesReportZeroFpsAndIdleTime() {
        ManagedSession.recordPresentedFrame(2_000_000_000L)
        ManagedSession.refreshFrameTelemetry(3_500_000_000L)

        val telemetry = ManagedSession.frameTelemetry.value
        assertEquals(0f, telemetry.fps, 0f)
        assertEquals(1500f, telemetry.frameTimeMs, 0.1f)
    }
}
