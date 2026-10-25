package io.paddrawboard.client.transport

import java.io.OutputStream

/** The desktop/Android transport preface, intentionally independent of v1 protocol framing. */
object SessionAuth {
    const val INTENT_EXTRA = "io.paddrawboard.client.extra.SESSION_TOKEN"
    const val TOKEN_HEX_LENGTH = 64
    const val TOKEN_BYTES = 32
    private val magic = byteArrayOf('P'.code.toByte(), 'D'.code.toByte(), 'B'.code.toByte(), 'A'.code.toByte(), 'U'.code.toByte(), 'T'.code.toByte(), 'H'.code.toByte(), '1'.code.toByte())

    enum class Channel(val id: Int) { CONTROL(1), VIDEO(2), INPUT(3) }

    /** Accepts exactly 64 ASCII hexadecimal characters and nothing else. */
    fun parseTokenHex(value: String?): ByteArray? {
        if (value == null || value.length != TOKEN_HEX_LENGTH) return null
        val token = ByteArray(TOKEN_BYTES)
        for (index in token.indices) {
            val high = hexNibble(value[index * 2]) ?: return null
            val low = hexNibble(value[index * 2 + 1]) ?: return null
            token[index] = ((high shl 4) or low).toByte()
        }
        return token
    }

    fun buildPreface(token: ByteArray, channel: Channel): ByteArray {
        require(token.size == TOKEN_BYTES) { "session token must be 32 bytes" }
        return ByteArray(magic.size + 1 + TOKEN_BYTES).also { preface ->
            magic.copyInto(preface)
            preface[magic.size] = channel.id.toByte()
            token.copyInto(preface, magic.size + 1)
        }
    }

    fun writePreface(output: OutputStream, token: ByteArray, channel: Channel) {
        output.write(buildPreface(token, channel))
        output.flush()
    }

    private fun hexNibble(value: Char): Int? = when (value) {
        in '0'..'9' -> value - '0'
        in 'a'..'f' -> value - 'a' + 10
        in 'A'..'F' -> value - 'A' + 10
        else -> null
    }
}
