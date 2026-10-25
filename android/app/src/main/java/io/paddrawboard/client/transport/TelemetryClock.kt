package io.paddrawboard.client.transport

import io.paddrawboard.protocol.PdbProtocol
import kotlin.math.max

/** NTP-style client clock/latency estimator for control-channel telemetry. */
class TelemetryClock(private val nowNs: () -> Long) {
    private var lastRttUs = 0L
    /** Signed host-minus-client clock offset: hostTime - clientTime. */
    private var hostMinusClientOffsetNs: Long? = null
    private var lastVideoLatencyUs = 0L

    @Synchronized fun request(timestampNs: Long = nowNs()): PdbProtocol.ClockSyncRequest =
        PdbProtocol.ClockSyncRequest(timestampNs)

    @Synchronized fun accept(response: PdbProtocol.ClockSyncResponse, receivedNs: Long = nowNs()) {
        val roundTripNs = max(0L, receivedNs - response.clientSendTimestampNs)
        lastRttUs = roundTripNs / 1_000L
        // offset = host - client = ((hostReceive-clientSend) + (hostSend-clientReceive)) / 2
        hostMinusClientOffsetNs = ((response.hostReceiveTimestampNs - response.clientSendTimestampNs) +
            (response.hostSendTimestampNs - receivedNs)) / 2L
    }

    /** Records t4 and returns the exact four-timestamp completion payload for the host. */
    @Synchronized fun acceptAndComplete(
        response: PdbProtocol.ClockSyncResponse,
        deviceReceiveNs: Long,
    ): PdbProtocol.ClockSyncComplete {
        accept(response, deviceReceiveNs)
        return PdbProtocol.ClockSyncComplete(
            response.clientSendTimestampNs,
            response.hostReceiveTimestampNs,
            response.hostSendTimestampNs,
            deviceReceiveNs,
        )
    }

    @Synchronized fun recordVideo(hostPresentationTimestampNs: Long, receivedNs: Long = nowNs()) {
        val offset = hostMinusClientOffsetNs ?: return
        // Convert host presentation time to the client clock: client = host - (host - client).
        val clientPresentationTimestampNs = hostPresentationTimestampNs - offset
        lastVideoLatencyUs = max(0L, receivedNs - clientPresentationTimestampNs) / 1_000L
    }

    @Synchronized fun telemetry(droppedVideoFrames: Long): PdbProtocol.Telemetry = PdbProtocol.Telemetry(
        lastRttUs.coerceIn(0L, 0xffff_ffffL),
        lastVideoLatencyUs.coerceIn(0L, 0xffff_ffffL),
        droppedVideoFrames.coerceIn(0L, 0xffff_ffffL),
    )
}
