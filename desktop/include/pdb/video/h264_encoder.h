#pragma once

#include "pdb/video/async_mft_state.h"
#include "pdb/video/types.h"

#include <mftransform.h>
#include <mfidl.h>
#include <mferror.h>

#include <chrono>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace pdb::video {

struct H264EncoderConfig {
  Size size;
  std::uint32_t frame_rate{60};
  std::uint32_t bitrate_bits_per_second{80'000'000};
};

struct H264EncoderCandidateDiagnostics {
  std::wstring name;
  std::uint32_t enumeration_index{};
  HRESULT setup_status{S_OK};
  HRESULT benchmark_status{S_OK};
  std::chrono::microseconds measured_latency{};
  std::uint32_t measured_frames{};
  bool asynchronous{};
  // True only for candidates returned by adapter-scoped MFTEnum2. An
  // unscoped hardware MFT must never win selection on a multi-GPU system.
  bool adapter_scoped{};
  bool usable{};
};

// Some hardware encoders expose optional CodecAPI properties but reject a
// SetValue call when the property is not implemented by that MFT. In that
// case the documented encoder default remains authoritative.
[[nodiscard]] constexpr bool IsOptionalH264CodecApiPropertyFailure(HRESULT hr) noexcept {
  return hr == E_INVALIDARG || hr == E_NOTIMPL || hr == E_NOINTERFACE ||
         hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

inline constexpr std::uint32_t kMaxH264EncoderOutputStreamChanges = 4;
inline constexpr std::uint32_t kMaxH264EncoderEmptyOutputRetries = 4;

enum class H264EncoderOutputAction : std::uint8_t {
  kConsumeOutput,
  kNeedMoreInput,
  kRetryOutput,
  kRenegotiateOutput,
  kFail,
};

enum class H264OutputSampleDisposition : std::uint8_t {
  kNone,
  kSameAsCaller,
  kReplacement,
};

enum class H264InputSampleCacheAction : std::uint8_t {
  kReuse,
  kRebuild,
  kRejectInFlight,
};

// The cached sample is mutable only after the MFT has consumed the previous
// ProcessInput. For async MFTs that is represented by !input_in_flight; for
// sync MFTs the caller invokes this decision only after ProcessOutput returns.
// Comparing IUnknown identities handles different interface pointer values for
// the same ID3D11Texture2D COM object.
[[nodiscard]] constexpr H264InputSampleCacheAction DecideH264InputSampleCacheAction(
    const void* cached_texture_identity, const void* frame_texture_identity,
    bool sample_ready, bool input_in_flight,
    bool mft_does_not_addref_input) noexcept {
  if (input_in_flight) return H264InputSampleCacheAction::kRejectInFlight;
  // ProcessInput explicitly permits reuse only when the MFT advertises
  // MFT_INPUT_STREAM_DOES_NOT_ADDREF. Without that flag, an output event is
  // not proof that the MFT has released the input sample.
  if (!mft_does_not_addref_input || !sample_ready ||
      cached_texture_identity != frame_texture_identity) {
    return H264InputSampleCacheAction::kRebuild;
  }
  return H264InputSampleCacheAction::kReuse;
}

// ProcessOutput does not add a reference for the caller-provided sample. A
// non-null, different returned pointer is a transferred reference owned by
// the caller; keep this decision independent of COM so all three ownership
// cases can be tested without a live MFT.
[[nodiscard]] constexpr H264OutputSampleDisposition ClassifyH264OutputSample(
    const void* caller_sample, const void* returned_sample) noexcept {
  if (returned_sample == nullptr) return H264OutputSampleDisposition::kNone;
  if (returned_sample == caller_sample) {
    return H264OutputSampleDisposition::kSameAsCaller;
  }
  return H264OutputSampleDisposition::kReplacement;
}

[[nodiscard]] constexpr bool H264OutputContainsSample(
    DWORD output_buffer_status) noexcept {
  return (output_buffer_status & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) == 0;
}

// ProcessOutput can report a format change after the first encoded sample,
// especially on Intel hardware MFTs. The pending output remains inside the
// transform and must be requested again after selecting an advertised output
// type. Bound the retries so a broken driver cannot spin forever.
[[nodiscard]] constexpr H264EncoderOutputAction ClassifyH264EncoderOutputStatus(
    HRESULT hr, DWORD output_buffer_status,
    std::uint32_t completed_stream_changes) noexcept {
  if (SUCCEEDED(hr)) return H264EncoderOutputAction::kConsumeOutput;
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return H264EncoderOutputAction::kNeedMoreInput;
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE &&
      completed_stream_changes < kMaxH264EncoderOutputStreamChanges) {
    constexpr DWORD kStreamStatusMask =
        MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE | MFT_OUTPUT_DATA_BUFFER_STREAM_END;
    const DWORD stream_status = output_buffer_status & kStreamStatusMask;
    if (stream_status == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
      return H264EncoderOutputAction::kRenegotiateOutput;
    }
    return H264EncoderOutputAction::kRetryOutput;
  }
  return H264EncoderOutputAction::kFail;
}

[[nodiscard]] constexpr bool ShouldRetryEmptyH264EncoderOutput(
    bool has_sample, std::uint32_t completed_empty_output_retries) noexcept {
  return !has_sample &&
         completed_empty_output_retries < kMaxH264EncoderEmptyOutputRetries;
}

// Benchmarking may leave a selected MFT with stream types that are no longer
// valid after the formal reset. Keep the required re-negotiation order small,
// pure, and directly testable.
enum class H264EncoderStreamSetupStep : std::uint8_t {
  kSetD3DManager,
  kConfigureCodecApi,
  kSetOutputType,
  kSetInputType,
  kBeginStreaming,
  kStartOfStream,
};

class H264EncoderStreamSetupSequence final {
 public:
  [[nodiscard]] bool Advance(H264EncoderStreamSetupStep step) noexcept {
    if (static_cast<std::uint8_t>(step) != next_step_) return false;
    ++next_step_;
    return true;
  }

  [[nodiscard]] bool complete() const noexcept {
    return next_step_ == kStepCount;
  }

  [[nodiscard]] bool media_types_configured() const noexcept {
    return next_step_ >=
           static_cast<std::uint8_t>(H264EncoderStreamSetupStep::kBeginStreaming);
  }

 private:
  static constexpr std::uint8_t kStepCount =
      static_cast<std::uint8_t>(H264EncoderStreamSetupStep::kStartOfStream) + 1;
  std::uint8_t next_step_{};
};

// Pure deterministic ordering used after runtime benchmarking. Unusable
// candidates never win; equal measurements prefer friendly name and then the
// stable MFT enumeration index.
[[nodiscard]] bool PreferH264EncoderCandidate(const H264EncoderCandidateDiagnostics& candidate,
                                              const H264EncoderCandidateDiagnostics& current) noexcept;

// Detects an IDR NAL unit in an Annex-B H.264 access unit. This is used as a
// fallback when an encoder omits MFSampleExtension_CleanPoint metadata.
[[nodiscard]] bool AnnexBAccessUnitContainsIdr(
    std::span<const std::uint8_t> access_unit) noexcept;

// CODECAPI_AVEncVideoForceKeyFrame is a ULONG (VT_UI4). A non-zero value
// requests that the next ProcessInput produce a key frame.
[[nodiscard]] VARIANT MakeForceKeyFrameVariant() noexcept;

class HardwareH264Encoder final {
 public:
  HardwareH264Encoder() = default;
  ~HardwareH264Encoder();
  HardwareH264Encoder(const HardwareH264Encoder&) = delete;
  HardwareH264Encoder& operator=(const HardwareH264Encoder&) = delete;

  // Selects a hardware NV12 -> H.264 MFT. Both synchronous and asynchronous
  // Media Foundation transforms are supported.
  [[nodiscard]] HRESULT Initialize(ID3D11Device* device, const H264EncoderConfig& config);
  void Shutdown();
  // Delivers asynchronous MFT events and any access unit already produced by
  // the encoder. S_FALSE means that no output is available yet.
  [[nodiscard]] HRESULT Poll(EncodedAccessUnit* output);
  [[nodiscard]] bool CanAcceptFrame() const noexcept;
  [[nodiscard]] bool HasPendingInput() const noexcept {
    return asynchronous_ && async_state_.HasPendingInput();
  }
  [[nodiscard]] std::optional<std::uint64_t> pending_input_sequence() const noexcept {
    return asynchronous_ ? async_state_.pending_sequence() : std::nullopt;
  }
  [[nodiscard]] HRESULT Encode(ID3D11Texture2D* nv12_frame, std::uint64_t sequence,
                               LONGLONG sample_time_100ns, EncodedAccessUnit* output);
  [[nodiscard]] HRESULT ResetAndRequestIdr();
  // An asynchronous fatal event requires the owner to reset the AVC stream at
  // its transport boundary. This consumes exactly one request.
  [[nodiscard]] bool TakeFatalRestartRequest() noexcept;
  [[nodiscard]] bool initialized() const noexcept { return transform_ != nullptr; }
  [[nodiscard]] const std::wstring& selected_name() const noexcept { return selected_name_; }
  [[nodiscard]] bool is_asynchronous() const noexcept { return asynchronous_; }
  [[nodiscard]] bool can_reuse_input_texture() const noexcept {
    return asynchronous_ && input_texture_reuse_allowed_;
  }
  [[nodiscard]] const std::vector<H264EncoderCandidateDiagnostics>& candidate_diagnostics() const noexcept {
    return candidate_diagnostics_;
  }
  [[nodiscard]] std::chrono::microseconds selected_benchmark_latency() const noexcept {
    return selected_benchmark_latency_;
  }
  [[nodiscard]] std::string_view last_failure_stage() const noexcept {
    return last_failure_stage_;
  }
  // Low-frequency initialization diagnostics. This includes the capture
  // adapter identity and the result of adapter-scoped MFT enumeration.
  [[nodiscard]] std::string_view adapter_diagnostic() const noexcept {
    return adapter_diagnostic_;
  }

 private:
  [[nodiscard]] HRESULT ConfigureTransform(const H264EncoderConfig& config);
  [[nodiscard]] HRESULT ReconfigureSelectedTransformForStream();
  [[nodiscard]] HRESULT RenegotiateOutputType();
  [[nodiscard]] HRESULT PerformFormalStreamReset();
  [[nodiscard]] HRESULT ConfigureCodecApi();
  [[nodiscard]] HRESULT ProcessOutput(std::uint64_t sequence, EncodedAccessUnit* output);
  [[nodiscard]] HRESULT PumpAsyncEvents();
  [[nodiscard]] HRESULT ProcessAsyncOutput(EncodedAccessUnit* output);
  [[nodiscard]] HRESULT BenchmarkCurrentCandidate(ID3D11Device* device,
                                                  const H264EncoderConfig& config,
                                                  std::chrono::microseconds* latency,
                                                  std::uint32_t* measured_frames);
  [[nodiscard]] HRESULT MarkAsyncFatal(HRESULT failure) noexcept;
  [[nodiscard]] HRESULT EnsureInputSample(ID3D11Texture2D* frame,
                                          LONGLONG sample_time_100ns);
  void ReleaseInputSampleObjects() noexcept;
  void ClearInputSample() noexcept;
  [[nodiscard]] HRESULT EnsureCallerOutputSample(DWORD cb_size);
  void ClearCallerOutputSample() noexcept;
  void ShutdownCurrentCandidate() noexcept;
  void BeginAsyncShutdown() noexcept;

  ComPtr<ID3D11Device> device_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT device_manager_reset_token_{};
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> event_generator_;
  // The DXGI buffer retains the texture. Reuse is legal only after the input
  // accepted by the previous ProcessInput has been fully consumed. Async MFTs
  // use input_sample_in_flight_; sync MFTs call ProcessOutput inline and leave
  // it false for the next Encode call.
  ComPtr<IMFSample> input_sample_;
  ComPtr<IMFMediaBuffer> input_buffer_;
  ComPtr<IUnknown> input_texture_identity_;
  bool input_sample_in_flight_{};
  // This is set only after media types are configured and GetInputStreamInfo
  // explicitly reports MFT_INPUT_STREAM_DOES_NOT_ADDREF. If absent, every
  // ProcessInput receives a fresh sample and no caller-side reuse occurs.
  bool input_sample_reuse_allowed_{};
  // A transform advertising HOLDS_BUFFERS may keep the submitted DXGI surface
  // after ProcessOutput; the converter must then allocate a distinct texture.
  bool input_texture_reuse_allowed_{};
  // Used only when the MFT requires caller-provided output samples. These
  // references are retained between completed ProcessOutput calls.
  ComPtr<IMFSample> caller_output_sample_;
  ComPtr<IMFMediaBuffer> caller_output_buffer_;
  DWORD caller_output_buffer_capacity_{};
  H264EncoderConfig config_{};
  std::wstring selected_name_;
  AsyncMftStateMachine async_state_;
  bool asynchronous_{};
  bool fatal_restart_requested_{};
  std::uint32_t consecutive_output_stream_changes_{};
  bool output_type_renegotiated_{};
  HRESULT last_async_fatal_{S_OK};
  bool mf_started_{};
  std::vector<H264EncoderCandidateDiagnostics> candidate_diagnostics_;
  std::chrono::microseconds selected_benchmark_latency_{};
  std::string adapter_diagnostic_;
  std::string last_failure_stage_{"none"};
};

}  // namespace pdb::video
