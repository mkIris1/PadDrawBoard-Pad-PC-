package io.paddrawboard.client.protocol

import io.paddrawboard.protocol.PdbProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import android.view.MotionEvent

class PdbCodecTest {
    @Test fun clockSyncCompleteUsesSharedOpcodeAndFourTimestamps() {
        val codec = PdbCodec()
        val expected = PdbProtocol.ClockSyncComplete(1L, 2L, 3L, 4L)
        val decoded = PdbProtocol.decode(PdbProtocol.encode(codec.control(expected, 9L))).payload
            as PdbProtocol.ControlPayload
        assertEquals(expected, decoded.control)
    }

    @Test fun clientHelloUsesSharedEncoder() {
        val codec = PdbCodec(); val encoded = ByteArrayOutputStream()
        codec.write(encoded, codec.hello(ClientCapabilities("Pad", "36", 3200, 2136, 90, true, true, false, false, false, true, emptySet()), 1))
        assertTrue(encoded.size() > PdbProtocol.HEADER_BYTES)
    }
    @Test fun serverConfigIsReadThroughSharedDecoder() {
        val codec = PdbCodec()
        val config = PdbProtocol.ServerConfig(1, 2, 3200, 2136, 60, 80, 0, 0, 3200, 2136, 0)
        val encoded = PdbProtocol.encode(PdbProtocol.Frame(PdbProtocol.Header(PdbProtocol.TYPE_SERVER_CONFIG, sequence = 1), config))
        val decoded = codec.read(ByteArrayInputStream(encoded)).payload as PdbProtocol.ServerConfig
        assertEquals(3200, decoded.videoWidth); assertEquals(2136, decoded.videoHeight)
    }
    @Test fun inputIsNormalizedAndUsesSharedFrameValidation() {
        val codec = PdbCodec(); val frame = codec.input(listOf(CapturedInputSample(20, 1, CapturedTool.STYLUS, CapturedAction.DOWN, 50f, 25f, .5f, 0f, 0f, 0f, 0)), 100, 50, 2)
        val decoded = PdbProtocol.decode(PdbProtocol.encode(frame)).payload as PdbProtocol.InputBatch
        assertEquals(32767, decoded.samples.single().x); assertEquals(32767, decoded.samples.single().y)
    }
    @Test fun inputHistoryRemainsOrdered() {
        val codec = PdbCodec()
        val samples = (0 until 3).map { index ->
            CapturedInputSample(index.toLong(), index + 1, CapturedTool.STYLUS, CapturedAction.MOVE,
                index.toFloat(), index.toFloat(), 0f, 0f, 0f, 0f, 0)
        }
        val decoded = PdbProtocol.decode(PdbProtocol.encode(codec.input(samples, 10, 10, 3))).payload as PdbProtocol.InputBatch
        assertEquals(listOf(0L, 1_000L, 2_000L), decoded.samples.map { it.timestampDeltaUs })
        assertEquals(listOf(1, 2, 3), decoded.samples.map { it.pointerId })
    }
    @Test(expected = IllegalArgumentException::class)
    fun rawAndroidButtonStateCannotPassAsProtocolBits() {
        val codec = PdbCodec()
        val frame = codec.input(listOf(CapturedInputSample(1, 1, CapturedTool.STYLUS, CapturedAction.DOWN,
            1f, 1f, 1f, 0f, 0f, 0f, MotionEvent.BUTTON_STYLUS_PRIMARY)), 10, 10, 4)
        PdbProtocol.encode(frame)
    }
}
