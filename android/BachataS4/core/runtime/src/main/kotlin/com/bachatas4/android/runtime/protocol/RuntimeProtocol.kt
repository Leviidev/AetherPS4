package com.bachatas4.android.runtime.protocol

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

const val PROTOCOL_VERSION = 1
const val MAX_FRAME_BYTES = 1_048_576

@Serializable
sealed interface RuntimeMessage {
    @Serializable
    @SerialName("hello")
    data class Hello(val version: Int, val runtimeVersion: String) : RuntimeMessage

    @Serializable
    @SerialName("state")
    data class State(val value: String, val detail: String = "") : RuntimeMessage

    @Serializable
    @SerialName("log")
    data class Log(val level: String, val category: String, val message: String) : RuntimeMessage

    @Serializable
    @SerialName("metrics")
    data class Metrics(val frameTimeMs: Double, val rssBytes: Long) : RuntimeMessage

    @Serializable
    @SerialName("stop")
    data object Stop : RuntimeMessage
}

class RuntimeProtocol(
    private val json: Json = Json {
        classDiscriminator = "type"
        encodeDefaults = true
    },
) {
    fun write(output: OutputStream, message: RuntimeMessage) {
        validate(message)
        val payload = json.encodeToString<RuntimeMessage>(message).encodeToByteArray()
        require(payload.size in 1..MAX_FRAME_BYTES) { "Invalid frame length: ${payload.size}" }
        DataOutputStream(output).apply {
            writeInt(payload.size)
            write(payload)
            flush()
        }
    }

    fun read(input: InputStream): RuntimeMessage {
        val data = DataInputStream(input)
        val length = data.readInt()
        require(length in 1..MAX_FRAME_BYTES) { "Invalid frame length: $length" }
        val payload = ByteArray(length)
        data.readFully(payload)
        val text = StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(payload))
            .toString()
        return json.decodeFromString<RuntimeMessage>(text).also(::validate)
    }

    private fun validate(message: RuntimeMessage) {
        if (message is RuntimeMessage.Hello) {
            require(message.version == PROTOCOL_VERSION) {
                "Unsupported protocol version: ${message.version}"
            }
        }
    }
}
