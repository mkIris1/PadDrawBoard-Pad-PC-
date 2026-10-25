package io.paddrawboard.client.transport

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionAuthTest {
    @Test fun parsesExactly64HexCharacters() {
        val parsed = SessionAuth.parseTokenHex("0123456789abcdef".repeat(4))
        requireNotNull(parsed)
        assertEquals(32, parsed.size)
        assertEquals(0x01.toByte(), parsed[0])
        assertEquals(0xef.toByte(), parsed[31])
        assertArrayEquals(parsed, SessionAuth.parseTokenHex("0123456789ABCDEF".repeat(4)))
        assertNull(SessionAuth.parseTokenHex("a".repeat(63)))
        assertNull(SessionAuth.parseTokenHex("a".repeat(65)))
        assertNull(SessionAuth.parseTokenHex("g".repeat(64)))
        assertNull(SessionAuth.parseTokenHex(" a" + "0".repeat(62)))
    }

    @Test fun prefaceContainsMagicChannelAndRawToken() {
        val token = ByteArray(32) { it.toByte() }
        val preface = SessionAuth.buildPreface(token, SessionAuth.Channel.VIDEO)
        assertEquals(41, preface.size)
        assertEquals('P'.code.toByte(), preface[0])
        assertEquals('1'.code.toByte(), preface[7])
        assertEquals(2.toByte(), preface[8])
        assertArrayEquals(token, preface.copyOfRange(9, 41))
        assertTrue(SessionAuth.buildPreface(token, SessionAuth.Channel.CONTROL)[8] == 1.toByte())
        assertFalse(SessionAuth.buildPreface(token, SessionAuth.Channel.INPUT)[8] == 1.toByte())
    }
}
