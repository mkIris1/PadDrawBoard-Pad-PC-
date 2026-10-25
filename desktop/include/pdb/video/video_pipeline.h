#pragma once

#include "pdb/video/backpressure_policy.h"
#include "pdb/video/d3d11_nv12_converter.h"
#include "pdb/video/desktop_duplication_capture.h"
#include "pdb/video/h264_encoder.h"
#include "pdb/video/latest_frame_queue.h"
#include "pdb/video/resolution_ladder.h"
#include "pdb/video/telemetry.h"

#include <string>
#include <string_view>
#include <vector>

namespace pdb::video {

enum class VideoLoopAfterEncodeAction {
  kSendOutput,
  kCapture,
  kFail,
};

[[nodiscard]] constexpr VideoLoopAfterEncodeAction DecideVideoLoopAfterInitialEncode(
    HRESULT encode_result) noexcept {
  if (encode_result == S_OK) return VideoLoopAfterEncodeAction::kSendOutput;
  if (encode_result == S_FALSE) return VideoLoopAfterEncodeAction::kCapture;
  return VideoLoopAfterEncodeAction::kFail;
}

[[nodiscard]] constexpr bool ShouldEncodeAfterCapture(HRESULT capture_result) noexcept {
  return capture_result == S_OK;
}

[[nodiscard]] constexpr bool ShouldAttemptDesktopCapture(bool has_pending_encode) noexcept {
  return !has_pending_encode;
}

[[nodiscard]] constexpr bool HasInFlightEncodeState(
    bool encoder_has_pending_input, bool telemetry_has_pending_encode) noexcept {
  return encoder_has_pending_input || telemetry_has_pending_encode;
}

[[nodiscard]] constexpr bool ShouldYieldAfterPendingEncodeNoProgress(
    HRESULT initial_encode_result, HRESULT capture_result,
    bool capture_was_skipped_for_pending) noexcept {
  return initial_encode_result == S_FALSE && capture_result == S_FALSE &&
         capture_was_skipped_for_pending;
}

// Owns the capture -> latest-only -> GPU conversion -> hardware encode path.
// Network transport remains outside this boundary and reports congestion through
// NotifyTransportWouldBlock() before it can discard any AVC reference frame.
class VideoPipeline final {
 public:
  [[nodiscard]] HRESULT Start(const MonitorInfo& monitor, std::uint32_t max_longest_edge,
                              VideoTelemetrySink* telemetry = nullptr);
  void Stop();
  [[nodiscard]] HRESULT CaptureOnce(DWORD timeout_ms);
  [[nodiscard]] bool has_pending_encode() const noexcept {
    return HasInFlightEncode();
  }
  // Pumps an asynchronous encoder before consuming another captured frame.
  // This prevents a pending raw frame from being discarded while hardware is
  // still working on the preceding access unit.
  [[nodiscard]] HRESULT EncodeLatest(EncodedAccessUnit* output);
  [[nodiscard]] TransportAction NotifyTransportWouldBlock();
  [[nodiscard]] Size encode_size() const noexcept { return encode_size_; }
  [[nodiscard]] std::uint32_t current_max_longest_edge() const noexcept {
    return policy_.current_max_longest_edge();
  }
  [[nodiscard]] const std::wstring& selected_encoder_name() const noexcept {
    return encoder_.selected_name();
  }
  [[nodiscard]] const std::vector<H264EncoderCandidateDiagnostics>& encoder_diagnostics() const noexcept {
    return encoder_.candidate_diagnostics();
  }
  [[nodiscard]] std::string_view last_failure_stage() const noexcept {
    return last_failure_stage_;
  }

 private:
  [[nodiscard]] bool HasInFlightEncode() const noexcept {
    return HasInFlightEncodeState(encoder_.HasPendingInput(), pending_encode_.has_pending());
  }

  [[nodiscard]] HRESULT CompleteEncodedFrame(EncodedAccessUnit& output);
  [[nodiscard]] HRESULT ResetEncoderAndRequestIdr(bool notify_telemetry);
  void ReportPolicy(const ResolutionPolicyObservation& observation,
                    const ResolutionPolicyDecision& decision);

  DesktopDuplicationCapture capture_;
  D3D11Nv12Converter converter_;
  HardwareH264Encoder encoder_;
  LatestFrameQueue<CapturedFrame> captured_frames_;
  EncodedBackpressurePolicy backpressure_;
  AdaptiveResolutionController policy_;
  Size encode_size_{};
  bool policy_reset_pending_{};
  PendingEncodeTelemetryState pending_encode_;
  VideoTelemetrySink* telemetry_{};  // Non-owning; caller controls lifetime.
  std::string last_failure_stage_{"none"};
};

}  // namespace pdb::video
