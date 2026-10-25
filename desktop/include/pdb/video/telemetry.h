#pragma once

#include "pdb/video/resolution_ladder.h"
#include "pdb/video/types.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace pdb::video {

struct CaptureTelemetry {
  std::uint64_t sequence{};
  std::chrono::microseconds acquire_duration{};
  std::chrono::microseconds gpu_copy_duration{};
  std::chrono::microseconds queue_age{};
  std::uint64_t replaced_frames{};
};

struct EncodeTelemetry {
  std::uint64_t sequence{};
  SteadyTime acquired_at{};
  SteadyTime encoded_at{};
  std::chrono::microseconds conversion_duration{};
  std::chrono::microseconds encode_duration{};
  std::chrono::microseconds capture_to_encode_duration{};
  bool idr{};
};

// Metadata retained between ProcessInput and a delayed asynchronous output.
// The encoded access unit is deliberately not retained here; the encoder owns
// that buffer until Poll returns it.
struct PendingEncodeTelemetry {
  std::uint64_t sequence{};
  SteadyTime acquired_at{};
  std::chrono::microseconds conversion_duration{};
  SteadyTime encode_started{};
};

enum class EncodeTelemetryCompletion {
  kNoPending,
  kCompleted,
  kSequenceMismatch,
};

// Pure, one-shot association of an encoded access unit with the input timing
// captured when ProcessInput accepted it. A mismatch consumes the pending
// record so a bad stream cannot cause repeated or incorrect callbacks.
class PendingEncodeTelemetryState final {
 public:
  [[nodiscard]] bool Begin(PendingEncodeTelemetry pending) noexcept {
    if (pending_.has_value()) return false;
    pending_ = pending;
    return true;
  }

  [[nodiscard]] bool has_pending() const noexcept { return pending_.has_value(); }

  void Clear() noexcept { pending_.reset(); }

  [[nodiscard]] EncodeTelemetryCompletion Complete(
      EncodedAccessUnit& output, SteadyTime completed_at,
      EncodeTelemetry* telemetry,
      std::chrono::microseconds* queue_age = nullptr) noexcept {
    if (telemetry == nullptr) return EncodeTelemetryCompletion::kNoPending;
    if (!pending_.has_value()) return EncodeTelemetryCompletion::kNoPending;
    const PendingEncodeTelemetry pending = *pending_;
    pending_.reset();
    if (pending.sequence != output.sequence) return EncodeTelemetryCompletion::kSequenceMismatch;

    output.acquired_at = pending.acquired_at;
    if (output.encoded_at == SteadyTime{}) output.encoded_at = completed_at;
    telemetry->sequence = output.sequence;
    telemetry->acquired_at = pending.acquired_at;
    telemetry->encoded_at = output.encoded_at;
    telemetry->conversion_duration = pending.conversion_duration;
    telemetry->encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        output.encoded_at - pending.encode_started);
    telemetry->capture_to_encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        output.encoded_at - pending.acquired_at);
    telemetry->idr = output.is_idr;
    if (queue_age != nullptr) {
      *queue_age = std::chrono::duration_cast<std::chrono::microseconds>(
          pending.encode_started - pending.acquired_at);
    }
    return EncodeTelemetryCompletion::kCompleted;
  }

 private:
  std::optional<PendingEncodeTelemetry> pending_;
};

struct ResolutionPolicyTelemetry {
  SteadyTime timestamp{};
  Size native_size{};
  Size encode_size{};
  std::uint32_t max_longest_edge{};
  std::chrono::microseconds queue_age{};
  std::chrono::microseconds encode_duration{};
  std::chrono::microseconds capture_to_encode_duration{};
  std::uint32_t consecutive_breaches{};
  std::chrono::milliseconds stable_duration{};
  bool transport_blocked{};
  bool release_budget_breached{};
  ResolutionPolicyAction action{ResolutionPolicyAction::kNone};
  bool request_stream_reset{};
};

class VideoTelemetrySink {
 public:
  virtual ~VideoTelemetrySink() = default;
  virtual void OnCapture(const CaptureTelemetry& telemetry) = 0;
  virtual void OnEncode(const EncodeTelemetry& telemetry) = 0;
  virtual void OnStreamResetRequested() = 0;
  // Optional so existing application sinks remain source-compatible.
  virtual void OnEncoderSelected(const std::wstring&, bool) {}
  // Optional so existing application sinks remain source-compatible. The
  // video layer calls this for every policy observation and decision.
  virtual void OnResolutionPolicy(const ResolutionPolicyTelemetry&) {}
};

}  // namespace pdb::video
