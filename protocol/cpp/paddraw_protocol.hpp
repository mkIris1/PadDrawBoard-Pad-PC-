#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace paddrawboard::protocol {

constexpr std::uint32_t kMagic = 0x31424450u;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kFrameHeaderBytes = 20;
constexpr std::uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u;
constexpr std::uint16_t kMaxInputSamples = 64;

enum class MessageType : std::uint16_t { ClientHello = 1, ServerConfig = 2, VideoFrame = 3, InputBatch = 4, Control = 5 };
enum Capability : std::uint32_t {
  kCapabilityPressure = 1u << 0, kCapabilityHover = 1u << 1, kCapabilityTilt = 1u << 2,
  kCapabilityDistance = 1u << 3, kCapabilityTouch = 1u << 4, kCapabilityButton1 = 1u << 5,
  kCapabilityButton2 = 1u << 6, kCapabilityButton3 = 1u << 7,
};
constexpr std::uint32_t kKnownCapabilities = 0xffu;
constexpr std::uint16_t kCodecH264 = 1u << 0;
constexpr std::uint32_t kVideoFlagIdr = 1u << 0;
constexpr std::uint32_t kConfigFlagSuppressTouchWhilePenInRange = 1u << 0;
constexpr std::uint16_t kDistanceUnavailable = 0xffffu;

enum class ToolType : std::uint8_t { Pen = 1, Touch = 2, Eraser = 3 };
enum ContactFlag : std::uint8_t { kInRange = 1u << 0, kContact = 1u << 1, kCancelled = 1u << 2, kPrimary = 1u << 3 };
constexpr std::uint8_t kKnownContactFlags = 0x0fu;
enum Button : std::uint16_t { kButton1 = 1u << 0, kButton2 = 1u << 1, kButton3 = 1u << 2 };
constexpr std::uint16_t kKnownButtons = 0x0007u;

enum class ControlOpcode : std::uint16_t { ClockSyncRequest = 1, ClockSyncResponse = 2, RequestIdr = 3, OrientationChanged = 4, CapabilityChanged = 5, Telemetry = 6, Disconnect = 7, ClockSyncComplete = 8 };

struct FrameHeader { std::uint32_t flags{}; std::uint32_t sequence{}; MessageType type{}; };
struct ClientHello { std::uint32_t capabilities{}; std::uint16_t displayWidth{}, displayHeight{}, rotationDegrees{}, maxTouchContacts{}, maxPenPressure{}, videoCodecMask{}; std::uint32_t maxVideoWidth{}, maxVideoHeight{}; std::string deviceName, osName; };
struct ServerConfig { std::uint64_t sessionId{}; std::uint32_t displayId{}; std::uint16_t videoWidth{}, videoHeight{}, videoFps{}, videoBitrateMbps{}; std::uint32_t contentLeft{}, contentTop{}, contentWidth{}, contentHeight{}, configFlags{}; };
struct VideoFrame { std::uint64_t captureTimestampNs{}, presentationTimestampNs{}; std::vector<std::uint8_t> accessUnit; };
struct InputSample { std::uint32_t timestampDeltaUs{}; std::uint16_t pointerId{}; ToolType tool{}; std::uint8_t contactFlags{}; std::uint16_t x{}, y{}, pressure{}; std::int16_t tiltX{}, tiltY{}; std::uint16_t distance{kDistanceUnavailable}, buttons{}; };
struct InputBatch { std::uint64_t batchTimestampNs{}; std::vector<InputSample> samples; };
struct ClockSyncRequest { std::uint64_t clientSendTimestampNs{}; };
struct ClockSyncResponse { std::uint64_t clientSendTimestampNs{}, hostReceiveTimestampNs{}, hostSendTimestampNs{}; };
struct ClockSyncComplete { std::uint64_t deviceSendNs{}, hostReceiveNs{}, hostSendNs{}, deviceReceiveNs{}; };
struct RequestIdr {};
struct OrientationChanged { std::uint16_t rotationDegrees{}; };
struct CapabilityChanged { std::uint32_t capabilities{}; };
struct Telemetry { std::uint32_t rttUs{}, videoLatencyUs{}, droppedVideoFrames{}; };
struct Disconnect { std::uint16_t reason{}; std::string message; };
using Control = std::variant<ClockSyncRequest, ClockSyncResponse, ClockSyncComplete, RequestIdr, OrientationChanged, CapabilityChanged, Telemetry, Disconnect>;
using Payload = std::variant<ClientHello, ServerConfig, VideoFrame, InputBatch, Control>;
struct Frame { FrameHeader header; Payload payload; };

struct ParseError { std::string message; };
template <typename T> using Result = std::variant<T, ParseError>;

std::vector<std::uint8_t> encodeFrame(const Frame& frame);
void encodeFrame(const Frame& frame, std::vector<std::uint8_t>& frame_output,
                 std::vector<std::uint8_t>& payload_scratch);
Result<Frame> decodeFrame(const std::uint8_t* bytes, std::size_t size);
inline Result<Frame> decodeFrame(const std::vector<std::uint8_t>& bytes) { return decodeFrame(bytes.data(), bytes.size()); }

}  // namespace paddrawboard::protocol
