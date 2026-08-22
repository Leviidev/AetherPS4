package com.bachatas4.android.runtime.session

import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameTelemetryReporterTest {
    @Test
    fun reportsAtMostOnceEveryTwoSeconds() {
        val reporter = FrameTelemetryReporter()

        assertNotNull(reporter.record(1_000_000_000L, FrameTelemetry(60f, 16.67f)))
        assertNull(reporter.record(2_999_999_999L, FrameTelemetry(58f, 17.24f)))
        val sample = reporter.record(3_000_000_000L, FrameTelemetry(55f, 18.18f))

        assertEquals(2_000L, sample?.elapsedMillis)
        assertEquals(55f, sample?.fps ?: 0f, 0f)
    }

    @Test
    fun resetStartsANewMonotonicCapture() {
        val reporter = FrameTelemetryReporter()
        reporter.record(5_000_000_000L, FrameTelemetry(30f, 33.33f))
        reporter.record(7_000_000_000L, FrameTelemetry(31f, 32.26f))

        reporter.reset()
        val sample = reporter.record(2_000_000_000L, FrameTelemetry(60f, 16.67f))

        assertEquals(0L, sample?.elapsedMillis)
    }

    @Test
    fun logLineUsesLocaleIndependentDecimals() {
        val previous = Locale.getDefault()
        Locale.setDefault(Locale.FRANCE)
        try {
            val sample = FrameTelemetrySample(2_000L, 30.5f, 32.79f)

            val line = sample.logLine()

            assertEquals("elapsedMs=2000 fps=30.50 frameTimeMs=32.79", line)
            assertTrue(',' !in line)
        } finally {
            Locale.setDefault(previous)
        }
    }
}
