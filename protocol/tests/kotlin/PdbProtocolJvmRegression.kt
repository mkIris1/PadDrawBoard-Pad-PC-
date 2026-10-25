package io.paddrawboard.protocol.tests

import io.paddrawboard.protocol.PdbProtocol
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class PdbProtocolJvmRegression {
    @Test
    fun clientHelloRoundTripsLargeU16AndU32Values() {
        val hello = PdbProtocol.ClientHello(
            capabilities = PdbProtocol.KNOWN_CAPABILITIES,
            displayWidth = 3200,
            displayHeight = 2136,
            rotationDegrees = 0,
            maxTouchContacts = 10,
            maxPenPressure = 8192,
            videoCodecMask = PdbProtocol.CODEC_H264,
            maxVideoWidth = 3200,
            maxVideoHeight = 2136,
            deviceName = "Xiaomi Pad 7",
            osName = "HyperOS 2",
        )
        val frame = PdbProtocol.Frame(
            PdbProtocol.Header(PdbProtocol.TYPE_CLIENT_HELLO, sequence = 0xfedc_ba98L),
            hello,
        )

        val encoded = PdbProtocol.encode(frame)
        val decoded = PdbProtocol.decode(encoded)
        assertEquals(0xfedc_ba98L, decoded.header.sequence)
        assertEquals(hello, decoded.payload)

        val goldenWithSequenceOne = "5044423101000100000000000100000031000000ff000000800c580800000a0000200100800c0000580800000c0009005869616f6d6920506164203748797065724f532032"
        val normalizedSequence = encoded.copyOf().also {
            it[12] = 1
            it[13] = 0
            it[14] = 0
            it[15] = 0
        }
        assertEquals(goldenWithSequenceOne, normalizedSequence.toHex())
    }

    @Test
    fun inputBatchRoundTripsLargeAxesAndSignedTilt() {
        val sample = PdbProtocol.InputSample(
            timestampDeltaUs = 70_000,
            pointerId = 513,
            tool = PdbProtocol.TOOL_PEN,
            contactFlags = PdbProtocol.CONTACT_IN_RANGE or PdbProtocol.CONTACT_CONTACT,
            x = 60_000,
            y = 40_000,
            pressure = 32_768,
            tiltX = -1_234,
            tiltY = 5_678,
            distance = 4_096,
            buttons = PdbProtocol.BUTTON_1 or PdbProtocol.BUTTON_3,
        )
        val batch = PdbProtocol.InputBatch(
            batchTimestampNs = 0x0102_0304_0506_0708L,
            samples = listOf(sample),
        )
        val frame = PdbProtocol.Frame(
            PdbProtocol.Header(PdbProtocol.TYPE_INPUT_BATCH, sequence = 0x89ab_cdefL),
            batch,
        )

        val decoded = PdbProtocol.decode(PdbProtocol.encode(frame))
        assertEquals(0x89ab_cdefL, decoded.header.sequence)
        assertEquals(batch, decoded.payload)
    }

    @Test
    fun clockSyncCompleteEncodingMatchesGoldenAndNtpFormula() {
        val complete = PdbProtocol.ClockSyncComplete(
            deviceSendNs = 1_000_000_000L,
            hostReceiveNs = 1_000_001_000L,
            hostSendNs = 1_000_002_000L,
            deviceReceiveNs = 1_000_005_000L,
        )
        val frame = PdbProtocol.Frame(
            PdbProtocol.Header(PdbProtocol.TYPE_CONTROL, sequence = 12L),
            PdbProtocol.ControlPayload(complete),
        )
        val encoded = PdbProtocol.encode(frame)
        assertEquals(
            "5044423101000500000000000c000000240000000800200000ca9a3b00000000e8cd9a3b00000000d0d19a3b0000000088dd9a3b00000000",
            encoded.toHex(),
        )
        assertEquals(complete, (PdbProtocol.decode(encoded).payload as PdbProtocol.ControlPayload).control)

        val offset = ((complete.hostReceiveNs - complete.deviceSendNs) + (complete.hostSendNs - complete.deviceReceiveNs)) / 2
        val delay = (complete.deviceReceiveNs - complete.deviceSendNs) - (complete.hostSendNs - complete.hostReceiveNs)
        assertEquals(-1_000L, offset)
        assertEquals(4_000L, delay)
    }

    @Test
    fun clockSyncCompleteRejectsMalformedBody() {
        val malformed = "5044423101000500000000000c000000240000000800200000ca9a3b00000000e8cd9a3b00000000d0d19a3b0000000088dd9a3b00000000"
            .chunked(2).map { it.toInt(16).toByte() }.toByteArray().also {
                it[22] = 0x18
                it[23] = 0
            }
        assertFailsWith<PdbProtocol.ProtocolException> { PdbProtocol.decode(malformed) }
    }

    private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xff) }
}
