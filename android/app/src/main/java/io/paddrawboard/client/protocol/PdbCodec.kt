package io.paddrawboard.client.protocol

import io.paddrawboard.protocol.PdbProtocol
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

data class ClientCapabilities(
    val deviceModel: String, val androidRelease: String, val width: Int, val height: Int, val rotationDegrees: Int,
    val h264Decoder: Boolean, val pressure: Boolean, val hover: Boolean, val tilt: Boolean, val distance: Boolean,
    val touch: Boolean, val penButtons: Set<Int>,
)
enum class CapturedTool { STYLUS, ERASER, FINGER }
enum class CapturedAction { DOWN, MOVE, UP, HOVER, CANCEL }
data class CapturedInputSample(
    val eventTimeMs: Long, val pointerId: Int, val tool: CapturedTool, val action: CapturedAction,
    val x: Float, val y: Float, val pressure: Float, val tilt: Float, val orientation: Float, val distance: Float, val buttons: Int,
)

/** Thin Android adapter; all v1 framing, constants and validation remain in protocol/kotlin. */
class PdbCodec {
    fun write(output: OutputStream, frame: PdbProtocol.Frame) { output.write(PdbProtocol.encode(frame)); output.flush() }
    fun read(input: InputStream): PdbProtocol.Frame {
        val header = ByteArray(PdbProtocol.HEADER_BYTES); input.readFully(header)
        val payloadLength = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).apply { position(PdbProtocol.HEADER_BYTES - Int.SIZE_BYTES) }.int.toLong() and 0xffff_ffffL
        require(payloadLength <= PdbProtocol.MAX_PAYLOAD_BYTES) { "protocol payload exceeds maximum" }
        val payload = ByteArray(payloadLength.toInt())
        input.readFully(payload)
        val all = header + payload
        return PdbProtocol.decode(all)
    }
    fun hello(c: ClientCapabilities, sequence: Long): PdbProtocol.Frame {
        val capabilities = capabilityBits(c)
        return frame(sequence, PdbProtocol.ClientHello(capabilities, c.width, c.height, c.rotationDegrees, if (c.touch) 10 else 0, 65535, PdbProtocol.CODEC_H264, c.width.toLong(), c.height.toLong(), c.deviceModel, "Android ${c.androidRelease}"))
    }

    fun withCapabilityBits(c: ClientCapabilities, bits: Long): ClientCapabilities = c.copy(
        pressure = bits and PdbProtocol.CAP_PRESSURE != 0L,
        hover = bits and PdbProtocol.CAP_HOVER != 0L,
        tilt = bits and PdbProtocol.CAP_TILT != 0L,
        distance = bits and PdbProtocol.CAP_DISTANCE != 0L,
        touch = bits and PdbProtocol.CAP_TOUCH != 0L,
        penButtons = buildSet {
            if (bits and PdbProtocol.CAP_BUTTON_1 != 0L) add(1)
            if (bits and PdbProtocol.CAP_BUTTON_2 != 0L) add(2)
            if (bits and PdbProtocol.CAP_BUTTON_3 != 0L) add(3)
        },
    )

    fun capabilityBits(c: ClientCapabilities): Long {
        var capabilities = 0L
        if (c.pressure) capabilities = capabilities or PdbProtocol.CAP_PRESSURE
        if (c.hover) capabilities = capabilities or PdbProtocol.CAP_HOVER
        if (c.tilt) capabilities = capabilities or PdbProtocol.CAP_TILT
        if (c.distance) capabilities = capabilities or PdbProtocol.CAP_DISTANCE
        if (c.touch) capabilities = capabilities or PdbProtocol.CAP_TOUCH
        if (c.penButtons.contains(1)) capabilities = capabilities or PdbProtocol.CAP_BUTTON_1
        if (c.penButtons.contains(2)) capabilities = capabilities or PdbProtocol.CAP_BUTTON_2
        if (c.penButtons.contains(3)) capabilities = capabilities or PdbProtocol.CAP_BUTTON_3
        return capabilities
    }
    fun input(samples: List<CapturedInputSample>, width: Int, height: Int, sequence: Long): PdbProtocol.Frame {
        require(samples.isNotEmpty())
        val firstMs = samples.first().eventTimeMs
        require(samples.size <= PdbProtocol.MAX_INPUT_SAMPLES) { "input history exceeds protocol batch limit" }
        val wire = samples.map { s ->
            val contact = when (s.action) {
                CapturedAction.CANCEL -> PdbProtocol.CONTACT_CANCELLED
                CapturedAction.UP -> PdbProtocol.CONTACT_IN_RANGE
                CapturedAction.HOVER -> PdbProtocol.CONTACT_IN_RANGE
                else -> PdbProtocol.CONTACT_IN_RANGE or PdbProtocol.CONTACT_CONTACT
            } or if (s.pointerId == samples.first().pointerId) PdbProtocol.CONTACT_PRIMARY else 0
            val tilt = (Math.toDegrees(s.tilt.toDouble()) * 100.0)
            PdbProtocol.InputSample(((s.eventTimeMs - firstMs).coerceAtLeast(0) * 1000), s.pointerId.coerceIn(1, 65535), tool(s.tool), contact,
                normalized(s.x, width), normalized(s.y, height), normalized(s.pressure, 1), (tilt * kotlin.math.cos(s.orientation)).toInt().coerceIn(-9000, 9000), (tilt * kotlin.math.sin(s.orientation)).toInt().coerceIn(-9000, 9000),
                (s.distance * 256f).toInt().coerceIn(0, 65535), s.buttons)
        }
        return frame(sequence, PdbProtocol.InputBatch(firstMs * 1_000_000L, wire))
    }
    fun control(control: PdbProtocol.Control, sequence: Long) = frame(sequence, PdbProtocol.ControlPayload(control))
    private fun frame(sequence: Long, payload: PdbProtocol.Payload) = PdbProtocol.Frame(PdbProtocol.Header(type(payload), 0, sequence), payload)
    private fun type(payload: PdbProtocol.Payload) = when (payload) { is PdbProtocol.ClientHello -> PdbProtocol.TYPE_CLIENT_HELLO; is PdbProtocol.InputBatch -> PdbProtocol.TYPE_INPUT_BATCH; is PdbProtocol.ControlPayload -> PdbProtocol.TYPE_CONTROL; is PdbProtocol.ServerConfig -> PdbProtocol.TYPE_SERVER_CONFIG; is PdbProtocol.VideoFrame -> PdbProtocol.TYPE_VIDEO_FRAME }
    private fun tool(tool: CapturedTool) = when (tool) { CapturedTool.STYLUS -> PdbProtocol.TOOL_PEN; CapturedTool.ERASER -> PdbProtocol.TOOL_ERASER; CapturedTool.FINGER -> PdbProtocol.TOOL_TOUCH }
    private fun normalized(value: Float, maximum: Int) = ((value / maximum.coerceAtLeast(1)) * 65535f).toInt().coerceIn(0, 65535)
    private fun InputStream.readFully(bytes: ByteArray) { var offset = 0; while (offset < bytes.size) { val count = read(bytes, offset, bytes.size - offset); if (count < 0) throw java.io.EOFException("truncated protocol frame"); offset += count } }
}
