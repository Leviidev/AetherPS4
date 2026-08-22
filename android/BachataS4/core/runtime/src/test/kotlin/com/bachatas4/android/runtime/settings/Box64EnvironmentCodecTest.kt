package com.bachatas4.android.runtime.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class Box64EnvironmentCodecTest {
    @Test
    fun decodeAcceptsCommentsAndEncodeIsDeterministic() {
        val decoded = Box64EnvironmentCodec.decode(
            """
            # performance
            BOX64_DYNAREC=1
            BOX64_LOG=0
            """.trimIndent(),
        )

        assertEquals(mapOf("BOX64_DYNAREC" to "1", "BOX64_LOG" to "0"), decoded)
        assertEquals("BOX64_DYNAREC=1\nBOX64_LOG=0\n", Box64EnvironmentCodec.encode(decoded))
    }

    @Test
    fun duplicateAndMalformedEntriesAreRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            Box64EnvironmentCodec.decode("BOX64_LOG=1\nBOX64_LOG=0")
        }
        assertThrows(IllegalArgumentException::class.java) {
            Box64EnvironmentCodec.decode("LD_LIBRARY_PATH=/tmp")
        }
        assertThrows(IllegalArgumentException::class.java) {
            Box64EnvironmentCodec.encode(mapOf("BOX64_LOG" to "bad\nvalue"))
        }
    }

    @Test
    fun launchOwnedEntriesAreRejected() {
        val error = assertThrows(IllegalArgumentException::class.java) {
            Box64EnvironmentCodec.decode("BOX64_PATH=/tmp")
        }
        assertTrue(error.message.orEmpty().contains("launch-owned"))
        assertThrows(IllegalArgumentException::class.java) {
            Box64EnvironmentCodec.decode("BOX64_PROFILE=fast")
        }
    }
}
