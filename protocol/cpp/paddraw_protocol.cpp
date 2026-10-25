#include "paddraw_protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace paddrawboard::protocol {
namespace {

class Writer {
 public:
  explicit Writer(std::vector<std::uint8_t>& bytes) : bytes_(bytes) { bytes_.clear(); }
  void u8(std::uint8_t value) { bytes_.push_back(value); }
  void u16(std::uint16_t value) { u8(static_cast<std::uint8_t>(value)); u8(static_cast<std::uint8_t>(value >> 8)); }
  void u32(std::uint32_t value) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(value >> (8 * i))); }
  void u64(std::uint64_t value) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(value >> (8 * i))); }
  void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }
  void bytes(const std::vector<std::uint8_t>& value) { append(value.data(), value.size()); }
  void string(const std::string& value) {
    append(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }
 private:
  void append(const std::uint8_t* data, std::size_t size) {
    if (size > bytes_.max_size() - bytes_.size()) throw std::length_error("protocol output too large");
    bytes_.reserve(bytes_.size() + size);
    for (std::size_t i = 0; i < size; ++i) bytes_.push_back(data[i]);
  }

  std::vector<std::uint8_t>& bytes_;
};

class Reader {
 public:
  Reader(const std::uint8_t* bytes, std::size_t size) : current_(bytes), end_(bytes + size) {}
  bool u8(std::uint8_t& value) { if (remaining() < 1) return false; value = *current_++; return true; }
  bool u16(std::uint16_t& value) { std::uint8_t a, b; if (!u8(a) || !u8(b)) return false; value = static_cast<std::uint16_t>(a | (b << 8)); return true; }
  bool u32(std::uint32_t& value) { value = 0; for (int i = 0; i < 4; ++i) { std::uint8_t b; if (!u8(b)) return false; value |= static_cast<std::uint32_t>(b) << (8 * i); } return true; }
  bool u64(std::uint64_t& value) { value = 0; for (int i = 0; i < 8; ++i) { std::uint8_t b; if (!u8(b)) return false; value |= static_cast<std::uint64_t>(b) << (8 * i); } return true; }
  bool i16(std::int16_t& value) { std::uint16_t raw; if (!u16(raw)) return false; value = static_cast<std::int16_t>(raw); return true; }
  bool string(std::size_t count, std::string& value) { if (remaining() < count) return false; value.assign(reinterpret_cast<const char*>(current_), count); current_ += count; return true; }
  bool bytes(std::size_t count, std::vector<std::uint8_t>& value) { if (remaining() < count) return false; value.assign(current_, current_ + count); current_ += count; return true; }
  std::size_t remaining() const { return static_cast<std::size_t>(end_ - current_); }
 private:
  const std::uint8_t* current_;
  const std::uint8_t* end_;
};

bool validUtf8(const std::string& text) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
  std::size_t i = 0;
  while (i < text.size()) {
    const std::uint8_t c = p[i++];
    if (c <= 0x7f) continue;
    int continuation = 0;
    std::uint32_t codepoint = 0;
    if ((c & 0xe0) == 0xc0) { continuation = 1; codepoint = c & 0x1f; if (codepoint == 0) return false; }
    else if ((c & 0xf0) == 0xe0) { continuation = 2; codepoint = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { continuation = 3; codepoint = c & 0x07; if (codepoint > 4) return false; }
    else return false;
    if (i + static_cast<std::size_t>(continuation) > text.size()) return false;
    for (int n = 0; n < continuation; ++n) { const std::uint8_t next = p[i++]; if ((next & 0xc0) != 0x80) return false; codepoint = (codepoint << 6) | (next & 0x3f); }
    if ((continuation == 1 && codepoint < 0x80) || (continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000) || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

bool validRotation(std::uint16_t value) { return value == 0 || value == 90 || value == 180 || value == 270; }
bool validTool(ToolType value) { return value == ToolType::Pen || value == ToolType::Touch || value == ToolType::Eraser; }
bool validType(MessageType value) { return value >= MessageType::ClientHello && value <= MessageType::Control; }
bool allowedFlags(MessageType type, std::uint32_t flags) { return type == MessageType::VideoFrame ? (flags & ~kVideoFlagIdr) == 0 : flags == 0; }

[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
void check(bool condition, const char* message) { if (!condition) invalid(message); }
void validateHello(const ClientHello& p) {
  check((p.capabilities & ~kKnownCapabilities) == 0, "unknown capability bit");
  check(p.displayWidth > 0 && p.displayHeight > 0 && validRotation(p.rotationDegrees), "invalid display");
  check(p.maxTouchContacts <= 10 && p.maxPenPressure != 0 && (p.videoCodecMask & kCodecH264) != 0, "invalid hello limits");
  check(p.maxVideoWidth > 0 && p.maxVideoHeight > 0 && p.deviceName.size() <= 64 && p.osName.size() <= 32, "invalid hello size");
  check(validUtf8(p.deviceName) && validUtf8(p.osName), "invalid UTF-8");
}
void validateConfig(const ServerConfig& p) {
  check(p.sessionId != 0 && p.videoWidth > 0 && p.videoHeight > 0 && p.videoFps >= 1 && p.videoFps <= 60, "invalid server config");
  check(p.videoBitrateMbps >= 20 && p.videoBitrateMbps <= 120 && p.contentWidth != 0 && p.contentHeight != 0, "invalid server config limits");
  check((p.configFlags & ~kConfigFlagSuppressTouchWhilePenInRange) == 0, "unknown config flag");
}
void validateVideo(const VideoFrame& p) { check(!p.accessUnit.empty() && p.accessUnit.size() <= kMaxPayloadBytes - 20, "invalid video access unit"); }
void validateInput(const InputBatch& p) {
  check(!p.samples.empty() && p.samples.size() <= kMaxInputSamples, "invalid input sample count");
  std::uint32_t previous = 0;
  for (std::size_t i = 0; i < p.samples.size(); ++i) {
    const auto& s = p.samples[i];
    check(s.pointerId != 0 && validTool(s.tool) && (s.contactFlags & ~kKnownContactFlags) == 0 && (s.buttons & ~kKnownButtons) == 0, "invalid input sample");
    check(s.tiltX >= -9000 && s.tiltX <= 9000 && s.tiltY >= -9000 && s.tiltY <= 9000, "invalid tilt");
    check(i == 0 || s.timestampDeltaUs >= previous, "non-monotonic input delta");
    previous = s.timestampDeltaUs;
  }
}
void validateControl(const Control& control) {
  std::visit([](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, OrientationChanged>) check(validRotation(value.rotationDegrees), "invalid orientation");
    else if constexpr (std::is_same_v<T, CapabilityChanged>) check((value.capabilities & ~kKnownCapabilities) == 0, "unknown capability bit");
    else if constexpr (std::is_same_v<T, Disconnect>) check(value.message.size() <= 128 && validUtf8(value.message), "invalid disconnect message");
  }, control);
}

std::vector<std::uint8_t> encodeControl(const Control& control) {
  validateControl(control);
  std::vector<std::uint8_t> bodyBytes;
  Writer body(bodyBytes);
  ControlOpcode opcode{};
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, ClockSyncRequest>) { opcode = ControlOpcode::ClockSyncRequest; body.u64(value.clientSendTimestampNs); }
    else if constexpr (std::is_same_v<T, ClockSyncResponse>) { opcode = ControlOpcode::ClockSyncResponse; body.u64(value.clientSendTimestampNs); body.u64(value.hostReceiveTimestampNs); body.u64(value.hostSendTimestampNs); }
    else if constexpr (std::is_same_v<T, ClockSyncComplete>) { opcode = ControlOpcode::ClockSyncComplete; body.u64(value.deviceSendNs); body.u64(value.hostReceiveNs); body.u64(value.hostSendNs); body.u64(value.deviceReceiveNs); }
    else if constexpr (std::is_same_v<T, RequestIdr>) { opcode = ControlOpcode::RequestIdr; }
    else if constexpr (std::is_same_v<T, OrientationChanged>) { opcode = ControlOpcode::OrientationChanged; body.u16(value.rotationDegrees); body.u16(0); }
    else if constexpr (std::is_same_v<T, CapabilityChanged>) { opcode = ControlOpcode::CapabilityChanged; body.u32(value.capabilities); }
    else if constexpr (std::is_same_v<T, Telemetry>) { opcode = ControlOpcode::Telemetry; body.u32(value.rttUs); body.u32(value.videoLatencyUs); body.u32(value.droppedVideoFrames); }
    else if constexpr (std::is_same_v<T, Disconnect>) { opcode = ControlOpcode::Disconnect; body.u16(value.reason); body.u16(static_cast<std::uint16_t>(value.message.size())); body.string(value.message); }
  }, control);
  std::vector<std::uint8_t> resultBytes;
  Writer result(resultBytes);
  result.u16(static_cast<std::uint16_t>(opcode));
  result.u16(static_cast<std::uint16_t>(bodyBytes.size()));
  result.bytes(bodyBytes);
  return resultBytes;
}

void encodePayload(const Payload& payload, MessageType& type,
                   std::vector<std::uint8_t>& output) {
  Writer w(output);
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, ClientHello>) { type = MessageType::ClientHello; validateHello(value); w.u32(value.capabilities); w.u16(value.displayWidth); w.u16(value.displayHeight); w.u16(value.rotationDegrees); w.u16(value.maxTouchContacts); w.u16(value.maxPenPressure); w.u16(value.videoCodecMask); w.u32(value.maxVideoWidth); w.u32(value.maxVideoHeight); w.u16(static_cast<std::uint16_t>(value.deviceName.size())); w.u16(static_cast<std::uint16_t>(value.osName.size())); w.string(value.deviceName); w.string(value.osName); }
    else if constexpr (std::is_same_v<T, ServerConfig>) { type = MessageType::ServerConfig; validateConfig(value); w.u64(value.sessionId); w.u32(value.displayId); w.u16(value.videoWidth); w.u16(value.videoHeight); w.u16(value.videoFps); w.u16(value.videoBitrateMbps); w.u32(value.contentLeft); w.u32(value.contentTop); w.u32(value.contentWidth); w.u32(value.contentHeight); w.u32(value.configFlags); }
    else if constexpr (std::is_same_v<T, VideoFrame>) { type = MessageType::VideoFrame; validateVideo(value); w.u64(value.captureTimestampNs); w.u64(value.presentationTimestampNs); w.u32(static_cast<std::uint32_t>(value.accessUnit.size())); w.bytes(value.accessUnit); }
    else if constexpr (std::is_same_v<T, InputBatch>) { type = MessageType::InputBatch; validateInput(value); w.u64(value.batchTimestampNs); w.u16(static_cast<std::uint16_t>(value.samples.size())); w.u16(0); for (const auto& s : value.samples) { w.u32(s.timestampDeltaUs); w.u16(s.pointerId); w.u8(static_cast<std::uint8_t>(s.tool)); w.u8(s.contactFlags); w.u16(s.x); w.u16(s.y); w.u16(s.pressure); w.i16(s.tiltX); w.i16(s.tiltY); w.u16(s.distance); w.u16(s.buttons); } }
    else if constexpr (std::is_same_v<T, Control>) { type = MessageType::Control; auto bytes = encodeControl(value); w.bytes(bytes); }
  }, payload);
}

Result<Payload> decodePayload(MessageType type, const std::uint8_t* bytes, std::size_t size) {
  Reader r(bytes, size);
  if (type == MessageType::ClientHello) {
    ClientHello p; std::uint16_t deviceBytes, osBytes;
    if (!r.u32(p.capabilities) || !r.u16(p.displayWidth) || !r.u16(p.displayHeight) || !r.u16(p.rotationDegrees) || !r.u16(p.maxTouchContacts) || !r.u16(p.maxPenPressure) || !r.u16(p.videoCodecMask) || !r.u32(p.maxVideoWidth) || !r.u32(p.maxVideoHeight) || !r.u16(deviceBytes) || !r.u16(osBytes) || deviceBytes > 64 || osBytes > 32 || !r.string(deviceBytes, p.deviceName) || !r.string(osBytes, p.osName) || r.remaining() != 0) return ParseError{"invalid ClientHello layout"};
    try { validateHello(p); } catch (const std::exception& e) { return ParseError{e.what()}; } return Payload{std::move(p)};
  }
  if (type == MessageType::ServerConfig) {
    ServerConfig p;
    if (size != 40 || !r.u64(p.sessionId) || !r.u32(p.displayId) || !r.u16(p.videoWidth) || !r.u16(p.videoHeight) || !r.u16(p.videoFps) || !r.u16(p.videoBitrateMbps) || !r.u32(p.contentLeft) || !r.u32(p.contentTop) || !r.u32(p.contentWidth) || !r.u32(p.contentHeight) || !r.u32(p.configFlags)) return ParseError{"invalid ServerConfig layout"};
    try { validateConfig(p); } catch (const std::exception& e) { return ParseError{e.what()}; } return Payload{p};
  }
  if (type == MessageType::VideoFrame) {
    VideoFrame p; std::uint32_t length;
    if (size < 20 || !r.u64(p.captureTimestampNs) || !r.u64(p.presentationTimestampNs) || !r.u32(length) || length != r.remaining() || !r.bytes(length, p.accessUnit)) return ParseError{"invalid VideoFrame layout"};
    try { validateVideo(p); } catch (const std::exception& e) { return ParseError{e.what()}; } return Payload{std::move(p)};
  }
  if (type == MessageType::InputBatch) {
    InputBatch p; std::uint16_t count, reserved;
    if (!r.u64(p.batchTimestampNs) || !r.u16(count) || !r.u16(reserved) || reserved != 0 || count == 0 || count > kMaxInputSamples || r.remaining() != static_cast<std::size_t>(count) * 22) return ParseError{"invalid InputBatch layout"};
    p.samples.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) { InputSample s; std::uint8_t tool; if (!r.u32(s.timestampDeltaUs) || !r.u16(s.pointerId) || !r.u8(tool) || !r.u8(s.contactFlags) || !r.u16(s.x) || !r.u16(s.y) || !r.u16(s.pressure) || !r.i16(s.tiltX) || !r.i16(s.tiltY) || !r.u16(s.distance) || !r.u16(s.buttons)) return ParseError{"truncated InputSample"}; s.tool = static_cast<ToolType>(tool); p.samples.push_back(s); }
    try { validateInput(p); } catch (const std::exception& e) { return ParseError{e.what()}; } return Payload{std::move(p)};
  }
  std::uint16_t rawOpcode, bodyLength;
  if (!r.u16(rawOpcode) || !r.u16(bodyLength) || bodyLength != r.remaining()) return ParseError{"invalid Control envelope"};
  const auto opcode = static_cast<ControlOpcode>(rawOpcode);
  auto exact = [&](std::size_t n) { return r.remaining() == n; };
  Control control;
  if (opcode == ControlOpcode::ClockSyncRequest) { ClockSyncRequest p; if (!exact(8) || !r.u64(p.clientSendTimestampNs)) return ParseError{"invalid ClockSyncRequest"}; control = p; }
  else if (opcode == ControlOpcode::ClockSyncResponse) { ClockSyncResponse p; if (!exact(24) || !r.u64(p.clientSendTimestampNs) || !r.u64(p.hostReceiveTimestampNs) || !r.u64(p.hostSendTimestampNs)) return ParseError{"invalid ClockSyncResponse"}; control = p; }
  else if (opcode == ControlOpcode::ClockSyncComplete) { ClockSyncComplete p; if (!exact(32) || !r.u64(p.deviceSendNs) || !r.u64(p.hostReceiveNs) || !r.u64(p.hostSendNs) || !r.u64(p.deviceReceiveNs)) return ParseError{"invalid ClockSyncComplete"}; control = p; }
  else if (opcode == ControlOpcode::RequestIdr) { if (!exact(0)) return ParseError{"invalid RequestIdr"}; control = RequestIdr{}; }
  else if (opcode == ControlOpcode::OrientationChanged) { OrientationChanged p; std::uint16_t reserved; if (!exact(4) || !r.u16(p.rotationDegrees) || !r.u16(reserved) || reserved != 0) return ParseError{"invalid OrientationChanged"}; control = p; }
  else if (opcode == ControlOpcode::CapabilityChanged) { CapabilityChanged p; if (!exact(4) || !r.u32(p.capabilities)) return ParseError{"invalid CapabilityChanged"}; control = p; }
  else if (opcode == ControlOpcode::Telemetry) { Telemetry p; if (!exact(12) || !r.u32(p.rttUs) || !r.u32(p.videoLatencyUs) || !r.u32(p.droppedVideoFrames)) return ParseError{"invalid Telemetry"}; control = p; }
  else if (opcode == ControlOpcode::Disconnect) { Disconnect p; std::uint16_t textBytes; if (r.remaining() < 4 || !r.u16(p.reason) || !r.u16(textBytes) || textBytes > 128 || textBytes != r.remaining() || !r.string(textBytes, p.message)) return ParseError{"invalid Disconnect"}; control = std::move(p); }
  else return ParseError{"unknown Control opcode"};
  try { validateControl(control); } catch (const std::exception& e) { return ParseError{e.what()}; }
  return Payload{std::move(control)};
}

}  // namespace

std::vector<std::uint8_t> encodeFrame(const Frame& frame) {
  std::vector<std::uint8_t> output;
  std::vector<std::uint8_t> payload_scratch;
  encodeFrame(frame, output, payload_scratch);
  return output;
}

void encodeFrame(const Frame& frame, std::vector<std::uint8_t>& frame_output,
                 std::vector<std::uint8_t>& payload_scratch) {
  frame_output.clear();
  payload_scratch.clear();
  MessageType payloadType{};
  encodePayload(frame.payload, payloadType, payload_scratch);
  check(frame.header.type == payloadType && allowedFlags(frame.header.type, frame.header.flags), "header and payload mismatch");
  check(payload_scratch.size() <= kMaxPayloadBytes, "payload too large");
  Writer writer(frame_output);
  writer.u32(kMagic); writer.u16(kVersion); writer.u16(static_cast<std::uint16_t>(frame.header.type)); writer.u32(frame.header.flags); writer.u32(frame.header.sequence); writer.u32(static_cast<std::uint32_t>(payload_scratch.size())); writer.bytes(payload_scratch);
}

Result<Frame> decodeFrame(const std::uint8_t* bytes, std::size_t size) {
  if (bytes == nullptr || size < kFrameHeaderBytes) return ParseError{"truncated frame header"};
  Reader r(bytes, size); std::uint32_t magic, flags, sequence, payloadLength; std::uint16_t version, rawType;
  if (!r.u32(magic) || !r.u16(version) || !r.u16(rawType) || !r.u32(flags) || !r.u32(sequence) || !r.u32(payloadLength)) return ParseError{"truncated frame header"};
  const auto type = static_cast<MessageType>(rawType);
  if (magic != kMagic) return ParseError{"wrong frame magic"};
  if (version != kVersion) return ParseError{"unsupported protocol version"};
  if (!validType(type)) return ParseError{"unknown message type"};
  if (!allowedFlags(type, flags)) return ParseError{"unknown message flag"};
  if (payloadLength > kMaxPayloadBytes || payloadLength != r.remaining()) return ParseError{"invalid payload length"};
  auto payload = decodePayload(type, bytes + kFrameHeaderBytes, payloadLength);
  if (std::holds_alternative<ParseError>(payload)) return std::get<ParseError>(std::move(payload));
  return Frame{FrameHeader{flags, sequence, type}, std::get<Payload>(std::move(payload))};
}

}  // namespace paddrawboard::protocol
