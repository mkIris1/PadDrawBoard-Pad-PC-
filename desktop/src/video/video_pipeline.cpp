#include "pdb/video/video_pipeline.h"

#include "pdb/video/resolution_ladder.h"

#include <mferror.h>

namespace pdb::video {

HRESULT VideoPipeline::Start(const MonitorInfo& monitor, std::uint32_t max_longest_edge,
                             VideoTelemetrySink* telemetry) {
  Stop();
  last_failure_stage_ = "capture_open";
  telemetry_ = telemetry;
  HRESULT hr = capture_.Open(monitor);
  if (FAILED(hr)) return hr;
  // Duplication reports its dimensions after the first acquired frame. Encoder
  // initialization therefore happens lazily in EncodeLatest.
  if (max_longest_edge != 3200 && max_longest_edge != 2560 && max_longest_edge != 1920) {
    Stop();
    return E_INVALIDARG;
  }
  policy_.Start(max_longest_edge);
  policy_reset_pending_ = false;
  last_failure_stage_ = "none";
  return S_OK;
}

void VideoPipeline::Stop() {
  pending_encode_.Clear();
  encoder_.Shutdown();
  converter_.Reset();
  capture_.Close();
  static_cast<void>(captured_frames_.TryPop());
  encode_size_ = {};
  policy_.Start(3200);
  policy_reset_pending_ = false;
  telemetry_ = nullptr;
  last_failure_stage_ = "none";
}

HRESULT VideoPipeline::ResetEncoderAndRequestIdr(bool notify_telemetry) {
  // MFT flush/restart invalidates any output that could complete the pending
  // input. Drop its timing before touching the encoder state.
  pending_encode_.Clear();
  last_failure_stage_ = "encoder_reset";
  const HRESULT hr = encoder_.ResetAndRequestIdr();
  if (SUCCEEDED(hr) && notify_telemetry && telemetry_) {
    telemetry_->OnStreamResetRequested();
  }
  return hr;
}

HRESULT VideoPipeline::CompleteEncodedFrame(EncodedAccessUnit& output) {
  EncodeTelemetry sample{};
  std::chrono::microseconds queue_age{};
  const auto completion = pending_encode_.Complete(
      output, std::chrono::steady_clock::now(), &sample, &queue_age);
  if (completion == EncodeTelemetryCompletion::kNoPending) return E_UNEXPECTED;
  if (completion == EncodeTelemetryCompletion::kSequenceMismatch) return E_UNEXPECTED;

  if (output.is_idr) backpressure_.OnFreshIdrProduced();
  if (telemetry_) telemetry_->OnEncode(sample);

  ResolutionPolicyObservation observation{};
  observation.timestamp = std::chrono::steady_clock::now();
  observation.queue_age = queue_age;
  observation.encode_duration = sample.encode_duration;
  observation.capture_to_encode_duration = sample.capture_to_encode_duration;
  ReportPolicy(observation, policy_.Observe(observation));
  return S_OK;
}

void VideoPipeline::ReportPolicy(const ResolutionPolicyObservation& observation,
                                 const ResolutionPolicyDecision& decision) {
  if (telemetry_) {
    ResolutionPolicyTelemetry sample{};
    sample.timestamp = observation.timestamp;
    sample.native_size = policy_.native_size();
    sample.encode_size = decision.encode_size;
    sample.max_longest_edge = decision.max_longest_edge;
    sample.queue_age = observation.queue_age;
    sample.encode_duration = observation.encode_duration;
    sample.capture_to_encode_duration = observation.capture_to_encode_duration;
    sample.consecutive_breaches = decision.consecutive_breaches;
    sample.stable_duration = decision.stable_duration;
    sample.transport_blocked = observation.transport_blocked;
    sample.release_budget_breached = decision.release_budget_breached;
    sample.action = decision.action;
    sample.request_stream_reset = decision.request_stream_reset;
    telemetry_->OnResolutionPolicy(sample);
  }
  if (decision.request_stream_reset) {
    policy_reset_pending_ = true;
    if (telemetry_) telemetry_->OnStreamResetRequested();
  }
}

HRESULT VideoPipeline::CaptureOnce(DWORD timeout_ms) {
  last_failure_stage_ = "capture";
  if (!ShouldAttemptDesktopCapture(HasInFlightEncode())) return S_FALSE;
  CapturedFrame frame;
  HRESULT failure = S_OK;
  const auto started = std::chrono::steady_clock::now();
  const auto result = capture_.AcquireLatest(&frame, timeout_ms, &failure);
  if (result == CaptureResult::kTimeout || result == CaptureResult::kRecovered) return S_FALSE;
  if (result != CaptureResult::kFrameAvailable) return failure;
  const auto sequence = frame.sequence;
  captured_frames_.Push(std::move(frame));
  if (telemetry_) {
    CaptureTelemetry sample{};
    sample.sequence = sequence;
    sample.acquire_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    sample.replaced_frames = captured_frames_.replaced_count();
    telemetry_->OnCapture(sample);
  }
  return S_OK;
}

HRESULT VideoPipeline::EncodeLatest(EncodedAccessUnit* output) {
  last_failure_stage_ = "encode_argument";
  if (output == nullptr) return E_POINTER;
  ResetEncodedAccessUnit(*output);

  // Async hardware MFTs can complete independently of the capture cadence.
  // Poll before consuming a raw frame so the one-slot latest-frame queue keeps
  // its newest sample until the transform explicitly requests another input.
  if (encoder_.initialized()) {
    last_failure_stage_ = "encoder_poll";
    const HRESULT poll = encoder_.Poll(output);
    if (poll == S_OK) {
      const HRESULT completion = CompleteEncodedFrame(*output);
      if (SUCCEEDED(completion)) return S_OK;
      const HRESULT reset = ResetEncoderAndRequestIdr(true);
      return FAILED(reset) ? reset : completion;
    }
    if (FAILED(poll)) {
      const bool restart_requested = encoder_.TakeFatalRestartRequest();
      const HRESULT reset = restart_requested ? ResetEncoderAndRequestIdr(true) : S_OK;
      if (!restart_requested) pending_encode_.Clear();
      return FAILED(reset) ? reset : poll;
    }
    if (!encoder_.CanAcceptFrame()) return S_FALSE;
  }

  auto frame = captured_frames_.TryPop();
  if (!frame) return S_FALSE;

  policy_.SetNativeSize(frame->size);
  const Size requested_size = policy_.current_encode_size();
  if (!requested_size.valid()) return E_INVALIDARG;
  const bool needs_reconfigure = !encoder_.initialized() || requested_size != encode_size_;
  if (needs_reconfigure) {
    pending_encode_.Clear();
    encoder_.Shutdown();
    converter_.Reset();
    const auto device = capture_.device();
    last_failure_stage_ = "capture_device";
    if (!device) return E_HANDLE;
    last_failure_stage_ = "converter_initialize";
    HRESULT hr = converter_.Initialize(device.Get());
    if (FAILED(hr)) return hr;
    H264EncoderConfig config{};
    config.size = requested_size;
    last_failure_stage_ = "encoder_initialize";
    hr = encoder_.Initialize(device.Get(), config);
    if (FAILED(hr)) {
      last_failure_stage_ += "/";
      last_failure_stage_ += encoder_.last_failure_stage();
      static_cast<void>(captured_frames_.PushIfEmpty(std::move(*frame)));
      return hr;
    }
    encode_size_ = requested_size;
    if (telemetry_) {
      telemetry_->OnEncoderSelected(encoder_.selected_name(),
                                    encoder_.is_asynchronous());
    }
    // Initialize() already performs the single formal stream reset and IDR
    // request. A second reset here can flush the freshly negotiated MFT again
    // immediately before the first ProcessInput.
    policy_reset_pending_ = false;
  } else if (policy_reset_pending_) {
    const HRESULT reset = ResetEncoderAndRequestIdr(false);
    if (FAILED(reset)) return reset;
    policy_reset_pending_ = false;
  }
  if (!encoder_.CanAcceptFrame()) {
    // The first NeedInput event can arrive after Initialize returns. Do not
    // lose this frame; a concurrently captured newer frame wins instead.
    static_cast<void>(captured_frames_.PushIfEmpty(std::move(*frame)));
    return S_FALSE;
  }
  ComPtr<ID3D11Texture2D> nv12;
  const auto conversion_started = std::chrono::steady_clock::now();
  last_failure_stage_ = "convert_bgra_to_nv12";
  HRESULT hr = converter_.Convert(frame->texture.Get(), encode_size_,
                                  encoder_.can_reuse_input_texture(), &nv12);
  if (FAILED(hr)) {
    last_failure_stage_ += "/";
    last_failure_stage_ += converter_.last_failure_stage();
    return hr;
  }
  const auto encode_started = std::chrono::steady_clock::now();
  PendingEncodeTelemetry pending{};
  pending.sequence = frame->sequence;
  pending.acquired_at = frame->acquired_at;
  pending.conversion_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      encode_started - conversion_started);
  pending.encode_started = encode_started;
  const LONGLONG sample_time = static_cast<LONGLONG>(frame->sequence - 1) * (10'000'000LL / 60);
  last_failure_stage_ = "encoder_encode";
  hr = encoder_.Encode(nv12.Get(), frame->sequence, sample_time, output);
  if (hr == S_FALSE) {
    if (encoder_.HasPendingInput()) {
      const auto accepted_sequence = encoder_.pending_input_sequence();
      if (!accepted_sequence.has_value() || *accepted_sequence != pending.sequence ||
          !pending_encode_.Begin(pending)) {
        pending_encode_.Clear();
        const HRESULT reset = ResetEncoderAndRequestIdr(true);
        return FAILED(reset) ? reset : E_UNEXPECTED;
      }
    }
    return S_FALSE;
  }
  if (hr == MF_E_NOTACCEPTING) {
    // A nonblocking async event race is transient and means ProcessInput did
    // not take ownership of the sample, so safely retain the latest raw frame.
    static_cast<void>(captured_frames_.PushIfEmpty(std::move(*frame)));
    return S_FALSE;
  }
  if (FAILED(hr)) {
    const bool restart_requested = encoder_.TakeFatalRestartRequest();
    pending_encode_.Clear();
    const HRESULT reset = restart_requested ? ResetEncoderAndRequestIdr(true) : S_OK;
    return FAILED(reset) ? reset : hr;
  }
  if (!pending_encode_.Begin(pending)) {
    pending_encode_.Clear();
    const HRESULT reset = ResetEncoderAndRequestIdr(true);
    return FAILED(reset) ? reset : E_UNEXPECTED;
  }
  const HRESULT completion = CompleteEncodedFrame(*output);
  if (SUCCEEDED(completion)) return S_OK;
  const HRESULT reset = ResetEncoderAndRequestIdr(true);
  return FAILED(reset) ? reset : completion;
}

TransportAction VideoPipeline::NotifyTransportWouldBlock() {
  const auto action = backpressure_.OnTransportWouldBlock();
  if (action == TransportAction::kRequestStreamReset) {
    const HRESULT hr = ResetEncoderAndRequestIdr(true);
    if (FAILED(hr)) return TransportAction::kRequestStreamReset;
  }
  ResolutionPolicyObservation observation{};
  observation.timestamp = std::chrono::steady_clock::now();
  observation.transport_blocked = true;
  const auto decision = policy_.Observe(observation);
  const bool already_reset = action == TransportAction::kRequestStreamReset;
  ReportPolicy(observation, decision);
  if (decision.request_stream_reset && !already_reset && encoder_.initialized()) {
    const HRESULT hr = ResetEncoderAndRequestIdr(false);
    if (FAILED(hr)) return TransportAction::kRequestStreamReset;
  }
  return action;
}

}  // namespace pdb::video
