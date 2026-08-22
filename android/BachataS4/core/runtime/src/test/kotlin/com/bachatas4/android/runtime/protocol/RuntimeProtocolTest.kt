package com.bachatas4.android.runtime.protocol

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class RuntimeProtocolTest {
    private val protocol = RuntimeProtocol()

    @Test
    fun roundTripsBigEndianFramedMessages() {
        val message = RuntimeMessage.Log("info", "runtime", "ready")
        val output = ByteArrayOutputStream()

        protocol.write(output, message)

        val frame = output.toByteArray()
        val payloadSize = frame.size - Int.SIZE_BYTES
        assertEquals(payloadSize, readBigEndianInt(frame))
        assertEquals(message, protocol.read(ByteArrayInputStream(frame)))
    }

    @Test
    fun roundTripsEveryMessageType() {
        val messages = listOf(
            RuntimeMessage.Hello(PROTOCOL_VERSION, "1.0.0"),
            RuntimeMessage.State("running", "CUSA00001"),
            RuntimeMessage.Log("warn", "gpu", "slow frame"),
            RuntimeMessage.Metrics(16.67, 512L),
            RuntimeMessage.Stop,
        )

        messages.forEach { message ->
            val output = ByteArrayOutputStream()
            protocol.write(output, message)
            assertEquals(message, protocol.read(ByteArrayInputStream(output.toByteArray())))
        }
    }

    @Test
    fun rejectsInvalidLengthsBeforeReadingPayload() {
        listOf(-1, 0, MAX_FRAME_BYTES + 1).forEach { length ->
            assertThrows(IllegalArgumentException::class.java) {
                protocol.read(lengthOnlyFrame(length))
            }
        }
    }

    @Test
    fun rejectsNonVersionOneHello() {
        assertThrows(IllegalArgumentException::class.java) {
            protocol.write(
                ByteArrayOutputStream(),
                RuntimeMessage.Hello(version = 2, runtimeVersion = "1.0.0"),
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            protocol.read(
                frame("""{"type":"hello","version":2,"runtimeVersion":"1.0.0"}"""),
            )
        }
    }

    private fun lengthOnlyFrame(length: Int): ByteArrayInputStream {
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).use { it.writeInt(length) }
        return ByteArrayInputStream(bytes.toByteArray())
    }

    private fun frame(json: String): ByteArrayInputStream {
        val payload = json.encodeToByteArray()
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).use {
            it.writeInt(payload.size)
            it.write(payload)
        }
        return ByteArrayInputStream(bytes.toByteArray())
    }

    private fun readBigEndianInt(bytes: ByteArray): Int =
        (bytes[0].toInt() and 0xff shl 24) or
            (bytes[1].toInt() and 0xff shl 16) or
            (bytes[2].toInt() and 0xff shl 8) or
            (bytes[3].toInt() and 0xff)
}
