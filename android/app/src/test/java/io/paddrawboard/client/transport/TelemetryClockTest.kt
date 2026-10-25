package io.paddrawboard.client.transport

import io.paddrawboard.protocol.PdbProtocol
import org.junit.Assert.assertEquals
import org.junit.Test

class TelemetryClockTest {
    @Test fun clockResponseProducesSerializedFourTimestampCompletion() {
        val clock = TelemetryClock { 1L }
        val response = PdbProtocol.ClockSyncResponse(
            clientSendTimestampNs = 1_000L,
            hostReceiveTimestampNs = 2_000L,
            hostSendTimestampNs = 3_000L,
        )
        assertEquals(
            PdbProtocol.ClockSyncComplete(1_000L, 2_000L, 3_000L, 4_000L),
            clock.acceptAndComplete(response, deviceReceiveNs = 4_000L),
        )
    }

    @Test fun clockResponseUpdatesRttAndVideoLatency() {
        val clock = TelemetryClock { 1_030_000_000L }
        clock.request(1_000_000_000L)
        clock.accept(PdbProtocol.ClockSyncResponse(
            clientSendTimestampNs = 1_000_000_000L,
            hostReceiveTimestampNs = 1_005_000_000L,
            hostSendTimestampNs = 1_006_000_000L,
        ), receivedNs = 1_030_000_000L)
        // The estimated host-minus-client offset is -9.5ms, so host presentation
        // time is converted to client time by subtracting that offset.
        clock.recordVideo(1_015_000_000L, receivedNs = 1_050_000_000L)
        assertEquals(PdbProtocol.Telemetry(30_000, 25_500, 7), clock.telemetry(7))
    }

    @Test fun telemetryIsSafeBeforeFirstClockResponse() {
        val clock = TelemetryClock { 1L }
        assertEquals(PdbProtocol.Telemetry(0, 0, 0), clock.telemetry(0))
        clock.recordVideo(10L, 20L)
        assertEquals(PdbProtocol.Telemetry(0, 0, 2), clock.telemetry(2))
    }
}
