#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pdb::app {

// These helpers deliberately emit only timing and health metadata. They never
// accept a pixel buffer or any other screen-content value.
class TelemetryJson final {
 public:
  [[nodiscard]] static std::string SessionEnd(std::string_view reason) {
    std::string escaped;
    escaped.reserve(reason.size());
    constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char character : reason) {
      switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
          if (character < 0x20) {
            escaped += "\\u00";
            escaped.push_back(kHex[(character >> 4) & 0x0f]);
            escaped.push_back(kHex[character & 0x0f]);
          } else {
            escaped.push_back(static_cast<char>(character));
          }
          break;
      }
    }
    return "{\"kind\":\"session_end\",\"reason\":\"" + escaped + "\"}";
  }

  [[nodiscard]] static std::string ClockSync(
      std::uint64_t device_send_ns, std::uint64_t host_receive_ns,
      std::uint64_t host_send_ns,
      std::optional<std::uint64_t> device_receive_ns = std::nullopt) {
    std::string line = "{\"kind\":\"clock_sync\",\"device_send_ns\":" +
                       std::to_string(device_send_ns) +
                       ",\"host_receive_ns\":" + std::to_string(host_receive_ns) +
                       ",\"host_send_ns\":" + std::to_string(host_send_ns);
    if (device_receive_ns.has_value()) {
      line += ",\"device_receive_ns\":" + std::to_string(*device_receive_ns);
    }
    line += ",\"complete\":" +
            std::string(device_receive_ns.has_value() ? "true" : "false") + "}";
    return line;
  }

  [[nodiscard]] static std::string InputTransport(
      std::uint64_t input_send_ns, std::uint64_t input_receive_ns,
      std::uint64_t latency_us, std::uint32_t active_pointers) {
    return "{\"kind\":\"input_transport\",\"input_send_ns\":" +
           std::to_string(input_send_ns) +
           ",\"input_receive_ns\":" + std::to_string(input_receive_ns) +
           ",\"latency_us\":" + std::to_string(latency_us) +
           ",\"active_pointers\":" + std::to_string(active_pointers) + "}";
  }

  [[nodiscard]] static std::string VideoSample(
      std::uint64_t capture_timestamp_ns, std::uint64_t presentation_timestamp_ns,
      std::uint64_t sequence, std::uint32_t queue_depth,
      std::uint64_t replaced_frames, std::uint64_t stream_resets) {
    return "{\"kind\":\"video_sample\",\"capture_timestamp_ns\":" +
           std::to_string(capture_timestamp_ns) +
           ",\"presentation_timestamp_ns\":" +
           std::to_string(presentation_timestamp_ns) +
           ",\"sequence\":" + std::to_string(sequence) +
           ",\"queue_depth\":" + std::to_string(queue_depth) +
           ",\"replaced_frames\":" + std::to_string(replaced_frames) +
           ",\"stream_resets\":" + std::to_string(stream_resets) + "}";
  }

  [[nodiscard]] static std::string Capture(
      std::uint64_t timestamp_ns, std::uint64_t sequence,
      std::uint64_t acquire_duration_us, std::uint64_t gpu_copy_duration_us,
      std::uint64_t queue_age_us, std::uint64_t replaced_frames) {
    return "{\"kind\":\"capture\",\"timestamp_ns\":" +
           std::to_string(timestamp_ns) + ",\"sequence\":" +
           std::to_string(sequence) + ",\"acquire_duration_us\":" +
           std::to_string(acquire_duration_us) + ",\"gpu_copy_duration_us\":" +
           std::to_string(gpu_copy_duration_us) + ",\"queue_age_us\":" +
           std::to_string(queue_age_us) + ",\"queue_depth\":0,\"replaced_frames\":" +
           std::to_string(replaced_frames) + "}";
  }

  [[nodiscard]] static std::string Encode(
      std::uint64_t timestamp_ns, std::uint64_t sequence,
      std::uint64_t conversion_duration_us, std::uint64_t encode_duration_us,
      std::uint64_t capture_to_encode_us, bool idr) {
    return "{\"kind\":\"video_encode\",\"timestamp_ns\":" +
           std::to_string(timestamp_ns) + ",\"sequence\":" +
           std::to_string(sequence) + ",\"conversion_duration_us\":" +
           std::to_string(conversion_duration_us) + ",\"encode_duration_us\":" +
           std::to_string(encode_duration_us) +
           ",\"capture_to_encode_us\":" + std::to_string(capture_to_encode_us) +
           ",\"idr\":" + std::string(idr ? "true" : "false") + "}";
  }

  [[nodiscard]] static std::string Soak(
      std::uint64_t timestamp_ns, std::uint32_t queue_depth,
      std::uint32_t active_pointers, std::uint64_t crashes,
      std::uint64_t stream_resets, std::optional<double> latency_ms = std::nullopt) {
    std::string line = "{\"kind\":\"soak\",\"timestamp_ns\":" +
                       std::to_string(timestamp_ns) +
                       ",\"queue_depth\":" + std::to_string(queue_depth) +
                       ",\"active_pointers\":" + std::to_string(active_pointers) +
                       ",\"crashes\":" + std::to_string(crashes) +
                       ",\"stream_resets\":" + std::to_string(stream_resets);
    if (latency_ms.has_value()) line += ",\"latency_ms\":" + std::to_string(*latency_ms);
    line += "}";
    return line;
  }
};

class TelemetryWriter final {
 public:
  explicit TelemetryWriter(std::filesystem::path path = DefaultPath())
      : path_(std::move(path)) {}

  void Write(std::string_view line);
  // A marker remains on disk only when the process exits without EndRun().
  // The returned count is cumulative across detected abnormal exits.
  [[nodiscard]] std::uint64_t BeginRun();
  void EndRun() noexcept;
  [[nodiscard]] bool CopyTo(const std::filesystem::path& destination,
                            std::string* error = nullptr) const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] static std::filesystem::path DefaultPath();

 private:
  static constexpr std::uintmax_t kMaximumBytes = 4u * 1024u * 1024u;
  static constexpr unsigned kRetainedRotations = 3;

  [[nodiscard]] bool OpenForAppend();
  void CloseStream() const noexcept;
  [[nodiscard]] bool FlushStream() const noexcept;
  [[nodiscard]] bool RotateIfNeeded(std::uintmax_t incoming_bytes);

  const std::filesystem::path path_;
  mutable std::mutex mutex_;
  mutable std::ofstream output_;
  mutable std::uintmax_t current_bytes_{};
  bool run_marker_active_{};
};

}  // namespace pdb::app
