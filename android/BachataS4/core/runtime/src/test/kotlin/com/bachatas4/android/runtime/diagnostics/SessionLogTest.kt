package com.bachatas4.android.runtime.diagnostics

import java.nio.file.Files
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionLogTest {
    @Test
    fun writesShadStyleLinesAndSanitizesSessionName() {
        val root = Files.createTempDirectory("session-log")
        val session = SessionLog.create(root, "GAME unsafe/id", Instant.parse("2026-07-04T12:34:56Z"), "abc123")

        session.info("Session", "driver=TURNIP_25_3_0_R11")
        session.error("Runtime", "backend exited")

        assertEquals("20260704-123456-GAME_unsafe_id-abc123", session.directory.fileName.toString())
        val text = Files.readString(session.applicationLog)
        assertTrue(text.contains("[App.Session] <Info> driver=TURNIP_25_3_0_R11"))
        assertTrue(text.contains("[App.Runtime] <Error> backend exited"))
    }

    @Test
    fun prunesOldestSessionDirectories() {
        val root = Files.createTempDirectory("session-log-retention")
        repeat(12) { index -> Files.createDirectory(root.resolve("session-${index.toString().padStart(2, '0')}")) }

        SessionLog.prune(root, keep = 10)

        assertFalse(Files.exists(root.resolve("session-00")))
        assertFalse(Files.exists(root.resolve("session-01")))
        assertTrue(Files.exists(root.resolve("session-11")))
        assertEquals(10, Files.list(root).use { it.count() })
    }
}
