@file:Suppress("MemberVisibilityCanBePrivate")

package io.paddrawboard.protocol

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

/** The sole Android-side definition of the PadDrawBoard v1 wire format. */
object PdbProtocol {
    const val MAGIC: Long = 0x31424450L
    const val VERSION = 1
    const val HEADER_BYTES = 20
    const val MAX_PAYLOAD_BYTES = 8 * 1024 * 1024
    const val MAX_INPUT_SAMPLES = 64

    const val TYPE_CLIENT_HELLO = 1
    const val TYPE_SERVER_CONFIG = 2
    const val TYPE_VIDEO_FRAME = 3
    const val TYPE_INPUT_BATCH = 4
    const val TYPE_CONTROL = 5

    const val CAP_PRESSURE: Long = 1L shl 0
    const val CAP_HOVER: Long = 1L shl 1
    const val CAP_TILT: Long = 1L shl 2
    const val CAP_DISTANCE: Long = 1L shl 3
    const val CAP_TOUCH: Long = 1L shl 4
    const val CAP_BUTTON_1: Long = 1L shl 5
    const val CAP_BUTTON_2: Long = 1L shl 6
    const val CAP_BUTTON_3: Long = 1L shl 7
    const val KNOWN_CAPABILITIES: Long = 0xffL
    const val CODEC_H264 = 1
    const val VIDEO_FLAG_IDR: Long = 1L
    const val CONFIG_FLAG_SUPPRESS_TOUCH_WHILE_PEN_IN_RANGE: Long = 1L
    const val DISTANCE_UNAVAILABLE = 0xffff

    const val TOOL_PEN = 1
    const val TOOL_TOUCH = 2
    const val TOOL_ERASER = 3
    const val CONTACT_IN_RANGE = 1
    const val CONTACT_CONTACT = 2
    const val CONTACT_CANCELLED = 4
    const val CONTACT_PRIMARY = 8
    const val KNOWN_CONTACT_FLAGS = 0x0f
    const val BUTTON_1 = 1
    const val BUTTON_2 = 2
    const val BUTTON_3 = 4
    const val KNOWN_BUTTONS = 7

    const val CONTROL_CLOCK_SYNC_REQUEST = 1
    const val CONTROL_CLOCK_SYNC_RESPONSE = 2
    const val CONTROL_REQUEST_IDR = 3
    const val CONTROL_ORIENTATION_CHANGED = 4
    const val CONTROL_CAPABILITY_CHANGED = 5
    const val CONTROL_TELEMETRY = 6
    const val CONTROL_DISCONNECT = 7
    const val CONTROL_CLOCK_SYNC_COMPLETE = 8

    class ProtocolException(message: String) : IllegalArgumentException(message)

    data class Header(val type: Int, val flags: Long = 0, val sequence: Long)
    sealed interface Payload
    data class ClientHello(
        val capabilities: Long, val displayWidth: Int, val displayHeight: Int,
        val rotationDegrees: Int, val maxTouchContacts: Int, val maxPenPressure: Int,
        val videoCodecMask: Int, val maxVideoWidth: Long, val maxVideoHeight: Long,
        val deviceName: String, val osName: String
    ) : Payload
    data class ServerConfig(
        val sessionId: Long, val displayId: Long, val videoWidth: Int, val videoHeight: Int,
        val videoFps: Int, val videoBitrateMbps: Int, val contentLeft: Long, val contentTop: Long,
        val contentWidth: Long, val contentHeight: Long, val configFlags: Long
    ) : Payload
    data class VideoFrame(val captureTimestampNs: Long, val presentationTimestampNs: Long, val accessUnit: ByteArray) : Payload
    data class InputSample(
        val timestampDeltaUs: Long, val pointerId: Int, val tool: Int, val contactFlags: Int,
        val x: Int, val y: Int, val pressure: Int, val tiltX: Int, val tiltY: Int,
        val distance: Int = DISTANCE_UNAVAILABLE, val buttons: Int = 0
    )
    data class InputBatch(val batchTimestampNs: Long, val samples: List<InputSample>) : Payload
    sealed interface Control
    data class ClockSyncRequest(val clientSendTimestampNs: Long) : Control
    data class ClockSyncResponse(val clientSendTimestampNs: Long, val hostReceiveTimestampNs: Long, val hostSendTimestampNs: Long) : Control
    data class ClockSyncComplete(val deviceSendNs: Long, val hostReceiveNs: Long, val hostSendNs: Long, val deviceReceiveNs: Long) : Control
    object RequestIdr : Control
    data class OrientationChanged(val rotationDegrees: Int) : Control
    data class CapabilityChanged(val capabilities: Long) : Control
    data class Telemetry(val rttUs: Long, val videoLatencyUs: Long, val droppedVideoFrames: Long) : Control
    data class Disconnect(val reason: Int, val message: String) : Control
    data class ControlPayload(val control: Control) : Payload
    data class Frame(val header: Header, val payload: Payload)

    fun encode(frame: Frame): ByteArray {
        val payloadType = typeOf(frame.payload)
        requireProtocol(frame.header.type == payloadType, "header and payload type differ")
        requireProtocol(allowedFlags(frame.header.type, frame.header.flags), "unknown message flag")
        val payload = encodePayload(frame.payload)
        requireProtocol(payload.size <= MAX_PAYLOAD_BYTES, "payload exceeds 8 MiB")
        return Writer().apply {
            u32(MAGIC); u16(VERSION); u16(frame.header.type); u32(frame.header.flags)
            u32(frame.header.sequence); u32(payload.size.toLong()); raw(payload)
        }.toByteArray()
    }

    /** Decodes exactly one complete frame; callers must retain incomplete stream bytes. */
    fun decode(bytes: ByteArray): Frame {
        requireProtocol(bytes.size >= HEADER_BYTES, "truncated frame header")
        val reader = Reader(bytes)
        val magic = reader.u32(); val version = reader.u16(); val type = reader.u16()
        val flags = reader.u32(); val sequence = reader.u32(); val payloadLength = reader.u32()
        requireProtocol(magic == MAGIC, "wrong frame magic")
        requireProtocol(version == VERSION, "unsupported protocol version")
        requireProtocol(type in TYPE_CLIENT_HELLO..TYPE_CONTROL, "unknown message type")
        requireProtocol(allowedFlags(type, flags), "unknown message flag")
        requireProtocol(payloadLength <= MAX_PAYLOAD_BYTES.toLong() && payloadLength == reader.remaining().toLong(), "invalid payload length")
        val payload = decodePayload(type, reader.bytes(payloadLength.toInt()))
        return Frame(Header(type, flags, sequence), payload)
    }

    private fun typeOf(payload: Payload) = when (payload) {
        is ClientHello -> TYPE_CLIENT_HELLO
        is ServerConfig -> TYPE_SERVER_CONFIG
        is VideoFrame -> TYPE_VIDEO_FRAME
        is InputBatch -> TYPE_INPUT_BATCH
        is ControlPayload -> TYPE_CONTROL
    }

    private fun encodePayload(payload: Payload): ByteArray = when (payload) {
        is ClientHello -> encodeHello(payload)
        is ServerConfig -> encodeConfig(payload)
        is VideoFrame -> encodeVideo(payload)
        is InputBatch -> encodeInput(payload)
        is ControlPayload -> encodeControl(payload.control)
    }

    private fun encodeHello(p: ClientHello): ByteArray {
        validateHello(p); val device = utf8(p.deviceName); val os = utf8(p.osName)
        return Writer().apply {
            u32(p.capabilities); u16(p.displayWidth); u16(p.displayHeight); u16(p.rotationDegrees)
            u16(p.maxTouchContacts); u16(p.maxPenPressure); u16(p.videoCodecMask); u32(p.maxVideoWidth); u32(p.maxVideoHeight)
            u16(device.size); u16(os.size); raw(device); raw(os)
        }.toByteArray()
    }

    private fun encodeConfig(p: ServerConfig): ByteArray {
        validateConfig(p)
        return Writer().apply {
            u64(p.sessionId); u32(p.displayId); u16(p.videoWidth); u16(p.videoHeight); u16(p.videoFps); u16(p.videoBitrateMbps)
            u32(p.contentLeft); u32(p.contentTop); u32(p.contentWidth); u32(p.contentHeight); u32(p.configFlags)
        }.toByteArray()
    }

    private fun encodeVideo(p: VideoFrame): ByteArray {
        requireProtocol(p.accessUnit.isNotEmpty() && p.accessUnit.size <= MAX_PAYLOAD_BYTES - 20, "invalid video access unit")
        return Writer().apply { u64(p.captureTimestampNs); u64(p.presentationTimestampNs); u32(p.accessUnit.size.toLong()); raw(p.accessUnit) }.toByteArray()
    }

    private fun encodeInput(p: InputBatch): ByteArray {
        validateInput(p)
        return Writer().apply {
            u64(p.batchTimestampNs); u16(p.samples.size); u16(0)
            p.samples.forEach { s ->
                u32(s.timestampDeltaUs); u16(s.pointerId); u8(s.tool); u8(s.contactFlags); u16(s.x); u16(s.y); u16(s.pressure)
                i16(s.tiltX); i16(s.tiltY); u16(s.distance); u16(s.buttons)
            }
        }.toByteArray()
    }

    private fun encodeControl(control: Control): ByteArray {
        validateControl(control)
        val opcode: Int; val body = Writer()
        when (control) {
            is ClockSyncRequest -> { opcode = CONTROL_CLOCK_SYNC_REQUEST; body.u64(control.clientSendTimestampNs) }
            is ClockSyncResponse -> { opcode = CONTROL_CLOCK_SYNC_RESPONSE; body.u64(control.clientSendTimestampNs); body.u64(control.hostReceiveTimestampNs); body.u64(control.hostSendTimestampNs) }
            is ClockSyncComplete -> { opcode = CONTROL_CLOCK_SYNC_COMPLETE; body.u64(control.deviceSendNs); body.u64(control.hostReceiveNs); body.u64(control.hostSendNs); body.u64(control.deviceReceiveNs) }
            RequestIdr -> opcode = CONTROL_REQUEST_IDR
            is OrientationChanged -> { opcode = CONTROL_ORIENTATION_CHANGED; body.u16(control.rotationDegrees); body.u16(0) }
            is CapabilityChanged -> { opcode = CONTROL_CAPABILITY_CHANGED; body.u32(control.capabilities) }
            is Telemetry -> { opcode = CONTROL_TELEMETRY; body.u32(control.rttUs); body.u32(control.videoLatencyUs); body.u32(control.droppedVideoFrames) }
            is Disconnect -> { opcode = CONTROL_DISCONNECT; val message = utf8(control.message); body.u16(control.reason); body.u16(message.size); body.raw(message) }
        }
        val bodyBytes = body.toByteArray()
        return Writer().apply { u16(opcode); u16(bodyBytes.size); raw(bodyBytes) }.toByteArray()
    }

    private fun decodePayload(type: Int, bytes: ByteArray): Payload = when (type) {
        TYPE_CLIENT_HELLO -> decodeHello(bytes)
        TYPE_SERVER_CONFIG -> decodeConfig(bytes)
        TYPE_VIDEO_FRAME -> decodeVideo(bytes)
        TYPE_INPUT_BATCH -> decodeInput(bytes)
        TYPE_CONTROL -> ControlPayload(decodeControl(bytes))
        else -> throw ProtocolException("unknown message type")
    }

    private fun decodeHello(bytes: ByteArray): ClientHello {
        val r = Reader(bytes)
        requireProtocol(bytes.size >= 28, "truncated ClientHello")
        val capabilities = r.u32()
        val displayWidth = r.u16()
        val displayHeight = r.u16()
        val rotationDegrees = r.u16()
        val maxTouchContacts = r.u16()
        val maxPenPressure = r.u16()
        val videoCodecMask = r.u16()
        val maxVideoWidth = r.u32()
        val maxVideoHeight = r.u32()
        val deviceNameBytes = r.u16()
        val osNameBytes = r.u16()
        requireProtocol(deviceNameBytes <= 64, "device name too long")
        requireProtocol(osNameBytes <= 32, "OS name too long")
        val p = ClientHello(
            capabilities, displayWidth, displayHeight, rotationDegrees,
            maxTouchContacts, maxPenPressure, videoCodecMask, maxVideoWidth,
            maxVideoHeight, r.utf8(deviceNameBytes), r.utf8(osNameBytes),
        )
        requireProtocol(r.remaining() == 0, "ClientHello trailing bytes"); validateHello(p); return p
    }

    private fun decodeConfig(bytes: ByteArray): ServerConfig {
        requireProtocol(bytes.size == 40, "invalid ServerConfig length"); val r = Reader(bytes)
        val p = ServerConfig(r.u64(), r.u32(), r.u16(), r.u16(), r.u16(), r.u16(), r.u32(), r.u32(), r.u32(), r.u32(), r.u32())
        validateConfig(p); return p
    }

    private fun decodeVideo(bytes: ByteArray): VideoFrame {
        requireProtocol(bytes.size >= 20, "truncated VideoFrame"); val r = Reader(bytes)
        val capture = r.u64(); val presentation = r.u64(); val length = r.u32()
        requireProtocol(length in 1..(MAX_PAYLOAD_BYTES - 20).toLong() && length == r.remaining().toLong(), "invalid access unit length")
        return VideoFrame(capture, presentation, r.bytes(length.toInt()))
    }

    private fun decodeInput(bytes: ByteArray): InputBatch {
        requireProtocol(bytes.size >= 12, "truncated InputBatch"); val r = Reader(bytes)
        val timestamp = r.u64(); val count = r.u16(); val reserved = r.u16()
        requireProtocol(count in 1..MAX_INPUT_SAMPLES && reserved == 0 && r.remaining() == count * 22, "invalid InputBatch layout")
        val samples = ArrayList<InputSample>(count)
        repeat(count) { samples += InputSample(r.u32(), r.u16(), r.u8(), r.u8(), r.u16(), r.u16(), r.u16(), r.i16(), r.i16(), r.u16(), r.u16()) }
        return InputBatch(timestamp, samples).also(::validateInput)
    }

    private fun decodeControl(bytes: ByteArray): Control {
        requireProtocol(bytes.size >= 4, "truncated Control"); val r = Reader(bytes); val opcode = r.u16(); val bodyBytes = r.u16()
        requireProtocol(bodyBytes == r.remaining(), "invalid Control body length")
        val control = when (opcode) {
            CONTROL_CLOCK_SYNC_REQUEST -> { requireProtocol(bodyBytes == 8, "invalid ClockSyncRequest"); ClockSyncRequest(r.u64()) }
            CONTROL_CLOCK_SYNC_RESPONSE -> { requireProtocol(bodyBytes == 24, "invalid ClockSyncResponse"); ClockSyncResponse(r.u64(), r.u64(), r.u64()) }
            CONTROL_CLOCK_SYNC_COMPLETE -> { requireProtocol(bodyBytes == 32, "invalid ClockSyncComplete"); ClockSyncComplete(r.u64(), r.u64(), r.u64(), r.u64()) }
            CONTROL_REQUEST_IDR -> { requireProtocol(bodyBytes == 0, "invalid RequestIdr"); RequestIdr }
            CONTROL_ORIENTATION_CHANGED -> { requireProtocol(bodyBytes == 4, "invalid OrientationChanged"); val rotation = r.u16(); requireProtocol(r.u16() == 0, "nonzero orientation reserved"); OrientationChanged(rotation) }
            CONTROL_CAPABILITY_CHANGED -> { requireProtocol(bodyBytes == 4, "invalid CapabilityChanged"); CapabilityChanged(r.u32()) }
            CONTROL_TELEMETRY -> { requireProtocol(bodyBytes == 12, "invalid Telemetry"); Telemetry(r.u32(), r.u32(), r.u32()) }
            CONTROL_DISCONNECT -> { requireProtocol(bodyBytes >= 4, "invalid Disconnect"); val reason = r.u16(); val messageBytes = r.u16(); requireProtocol(messageBytes <= 128 && messageBytes == r.remaining(), "invalid Disconnect message"); Disconnect(reason, r.utf8(messageBytes)) }
            else -> throw ProtocolException("unknown Control opcode")
        }
        requireProtocol(r.remaining() == 0, "Control trailing bytes"); validateControl(control); return control
    }

    private fun validateHello(p: ClientHello) {
        requireProtocol(p.capabilities and KNOWN_CAPABILITIES == p.capabilities, "unknown capability bit")
        requireProtocol(p.displayWidth > 0 && p.displayHeight > 0 && validRotation(p.rotationDegrees), "invalid display")
        requireProtocol(p.maxTouchContacts in 0..10 && p.maxPenPressure in 1..0xffff && p.videoCodecMask and CODEC_H264 != 0, "invalid hello limits")
        requireProtocol(p.maxVideoWidth > 0 && p.maxVideoHeight > 0 && utf8(p.deviceName).size <= 64 && utf8(p.osName).size <= 32, "invalid hello size")
    }
    private fun validateConfig(p: ServerConfig) {
        requireProtocol(p.sessionId != 0L && p.videoWidth > 0 && p.videoHeight > 0 && p.videoFps in 1..60 && p.videoBitrateMbps in 20..120, "invalid ServerConfig")
        requireProtocol(p.contentWidth > 0 && p.contentHeight > 0 && p.configFlags and CONFIG_FLAG_SUPPRESS_TOUCH_WHILE_PEN_IN_RANGE == p.configFlags, "invalid ServerConfig flags")
    }
    private fun validateInput(p: InputBatch) {
        requireProtocol(p.samples.size in 1..MAX_INPUT_SAMPLES, "invalid sample count"); var previous = 0L
        p.samples.forEachIndexed { index, s ->
            requireProtocol(s.timestampDeltaUs in 0..0xffff_ffffL && (index == 0 || s.timestampDeltaUs >= previous), "non-monotonic timestamp delta")
            requireProtocol(s.pointerId in 1..0xffff && s.tool in TOOL_PEN..TOOL_ERASER && s.contactFlags and KNOWN_CONTACT_FLAGS == s.contactFlags && s.buttons and KNOWN_BUTTONS == s.buttons, "invalid input sample")
            requireProtocol(s.x in 0..0xffff && s.y in 0..0xffff && s.pressure in 0..0xffff && s.distance in 0..0xffff && s.tiltX in -9000..9000 && s.tiltY in -9000..9000, "input axis out of range")
            previous = s.timestampDeltaUs
        }
    }
    private fun validateControl(control: Control) = when (control) {
        is OrientationChanged -> requireProtocol(validRotation(control.rotationDegrees), "invalid orientation")
        is CapabilityChanged -> requireProtocol(control.capabilities and KNOWN_CAPABILITIES == control.capabilities, "unknown capability bit")
        is Disconnect -> requireProtocol(control.reason in 0..0xffff && utf8(control.message).size <= 128, "invalid disconnect")
        else -> Unit
    }
    private fun validRotation(value: Int) = value == 0 || value == 90 || value == 180 || value == 270
    private fun allowedFlags(type: Int, flags: Long) = if (type == TYPE_VIDEO_FRAME) (flags and VIDEO_FLAG_IDR.inv()) == 0L else flags == 0L
    private fun requireProtocol(value: Boolean, message: String) { if (!value) throw ProtocolException(message) }
    private fun utf8(value: String): ByteArray { requireProtocol(validSurrogates(value), "invalid Unicode string"); return value.toByteArray(StandardCharsets.UTF_8) }
    private fun validSurrogates(value: String): Boolean { var index = 0; while (index < value.length) { val c = value[index]; if (Character.isHighSurrogate(c)) { if (index + 1 >= value.length || !Character.isLowSurrogate(value[index + 1])) return false; index += 2 } else { if (Character.isLowSurrogate(c)) return false; index++ } }; return true }

    private class Writer {
        private val stream = ByteArrayOutputStream()
        fun u8(value: Int) { requireProtocol(value in 0..0xff, "u8 out of range"); stream.write(value) }
        fun u16(value: Int) { requireProtocol(value in 0..0xffff, "u16 out of range"); u8(value and 0xff); u8((value ushr 8) and 0xff) }
        fun i16(value: Int) { requireProtocol(value in Short.MIN_VALUE..Short.MAX_VALUE, "i16 out of range"); u16(value and 0xffff) }
        fun u32(value: Long) { requireProtocol(value in 0..0xffff_ffffL, "u32 out of range"); repeat(4) { u8(((value ushr (it * 8)) and 0xffL).toInt()) } }
        fun u64(value: Long) { repeat(8) { u8(((value ushr (it * 8)) and 0xffL).toInt()) } }
        fun raw(value: ByteArray) { stream.write(value) }
        fun toByteArray(): ByteArray = stream.toByteArray()
    }
    private class Reader(private val bytes: ByteArray) {
        private var position = 0
        fun remaining() = bytes.size - position
        fun u8(): Int { require(1); return bytes[position++].toInt() and 0xff }
        fun u16(): Int = u8() or (u8() shl 8)
        fun i16(): Int = u16().toShort().toInt()
        fun u32(): Long { var value = 0L; repeat(4) { value = value or (u8().toLong() shl (it * 8)) }; return value }
        fun u64(): Long { var value = 0L; repeat(8) { value = value or (u8().toLong() shl (it * 8)) }; return value }
        fun bytes(count: Int): ByteArray { requireProtocol(count >= 0 && count <= remaining(), "truncated payload"); return bytes.copyOfRange(position, position + count).also { position += count } }
        fun utf8(count: Int): String = try { StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes(count))).toString() } catch (_: Exception) { throw ProtocolException("invalid UTF-8") }
        private fun require(count: Int) { requireProtocol(count <= remaining(), "truncated payload") }
    }
}
