#include "pdb/video/async_mft_state.h"
#include "pdb/video/backpressure_policy.h"
#include "pdb/video/desktop_duplication_capture.h"
#include "pdb/video/h264_encoder.h"
#include "pdb/video/latest_frame_queue.h"
#include "pdb/video/monitor_enumerator.h"
#include "pdb/video/resolution_ladder.h"
#include "pdb/video/telemetry.h"
#include "pdb/video/video_pipeline.h"

#include <cassert>
#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

namespace pdb::video::test {

void TransferredComReferencesAreAdoptedWithoutAddRef() {
  struct Counted {
    ULONG references{1};
    ULONG AddRef() { return ++references; }
    ULONG Release() { assert(references > 0); return --references; }
  } first, second;
  ComPtr<Counted> owner;
  AdoptComReference(owner, &first);
  assert(first.references == 1);
  first.AddRef();  // A second API-transferred reference to the same object.
  AdoptComReference(owner, &first);
  assert(first.references == 1);
  AdoptComReference(owner, &second);
  assert(first.references == 0 && second.references == 1);
  owner.Reset();
  assert(second.references == 0);
}

void OutputSampleDispositionDistinguishesCallerAndReplacement() {
  int caller_storage = 0;
  int replacement_storage = 0;
  const void* caller = &caller_storage;
  const void* replacement = &replacement_storage;

  assert(ClassifyH264OutputSample(nullptr, nullptr) ==
         H264OutputSampleDisposition::kNone);
  assert(ClassifyH264OutputSample(caller, caller) ==
         H264OutputSampleDisposition::kSameAsCaller);
  assert(ClassifyH264OutputSample(caller, replacement) ==
         H264OutputSampleDisposition::kReplacement);
  assert(ClassifyH264OutputSample(nullptr, replacement) ==
         H264OutputSampleDisposition::kReplacement);
  assert(H264OutputContainsSample(0));
  assert(!H264OutputContainsSample(MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE));
}

void InputSampleCachePolicyRequiresCompletedInputAndMatchingTexture() {
  int cached_texture = 0;
  int same_texture = 0;
  int other_texture = 0;
  assert(DecideH264InputSampleCacheAction(nullptr, &same_texture, false, false, true) ==
         H264InputSampleCacheAction::kRebuild);
  assert(DecideH264InputSampleCacheAction(&cached_texture, &same_texture, true, false, true) ==
         H264InputSampleCacheAction::kRebuild);
  assert(DecideH264InputSampleCacheAction(&cached_texture, &other_texture, true, false, true) ==
         H264InputSampleCacheAction::kRebuild);
  assert(DecideH264InputSampleCacheAction(&cached_texture, &cached_texture, true, false, true) ==
         H264InputSampleCacheAction::kReuse);
  assert(DecideH264InputSampleCacheAction(&cached_texture, &cached_texture, true, true, true) ==
         H264InputSampleCacheAction::kRejectInFlight);
  assert(DecideH264InputSampleCacheAction(&cached_texture, &cached_texture, true, false, false) ==
         H264InputSampleCacheAction::kRebuild);
}

void ResetEncodedAccessUnitRetainsByteCapacity() {
  EncodedAccessUnit output{};
  output.bytes.resize(64 * 1024);
  const auto capacity = output.bytes.capacity();
  output.sequence = 42;
  output.is_idr = true;
  output.acquired_at = SteadyTime{} + std::chrono::seconds(1);
  output.encoded_at = SteadyTime{} + std::chrono::seconds(2);

  ResetEncodedAccessUnit(output);

  assert(output.bytes.empty());
  assert(output.bytes.capacity() >= capacity);
  assert(output.sequence == 0);
  assert(!output.is_idr);
  assert(output.acquired_at == SteadyTime{});
  assert(output.encoded_at == SteadyTime{});
}

void PrivateCopyTextureDescriptionSupportsVideoProcessorInputView() {
  D3D11_TEXTURE2D_DESC source{};
  source.Width = 3200;
  source.Height = 2136;
  source.MipLevels = 4;
  source.ArraySize = 2;
  source.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  source.SampleDesc.Count = 8;
  source.SampleDesc.Quality = 7;
  source.Usage = D3D11_USAGE_STAGING;
  source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  source.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  const auto normalized = NormalizePrivateCopyTextureDesc(source);
  assert(normalized.Width == source.Width);
  assert(normalized.Height == source.Height);
  assert(normalized.MipLevels == source.MipLevels);
  assert(normalized.ArraySize == source.ArraySize);
  assert(normalized.Format == source.Format);
  assert(normalized.BindFlags ==
         (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
  assert(normalized.Usage == D3D11_USAGE_DEFAULT);
  assert(normalized.CPUAccessFlags == 0);
  assert(normalized.MiscFlags == 0);
  assert(normalized.SampleDesc.Count == 1);
  assert(normalized.SampleDesc.Quality == 0);
}

void PrivateCopyTextureCachePolicyReusesOnlyMatchingNormalizedDescription() {
  D3D11_TEXTURE2D_DESC source{};
  source.Width = 1920;
  source.Height = 1080;
  source.MipLevels = 1;
  source.ArraySize = 1;
  source.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  source.SampleDesc.Count = 1;
  source.Usage = D3D11_USAGE_STAGING;
  source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  source.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  const auto cached = NormalizePrivateCopyTextureDesc(source);
  assert(ShouldReusePrivateCopyTexture(cached, source));

  auto resized = source;
  resized.Width = 2560;
  assert(!ShouldReusePrivateCopyTexture(cached, resized));

  auto reformatted = source;
  reformatted.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  assert(!ShouldReusePrivateCopyTexture(cached, reformatted));

  // Source usage/bind/access flags are normalized before comparison, so
  // metadata changes that do not affect the private copy do not allocate.
  auto normalized_variant = source;
  normalized_variant.Usage = D3D11_USAGE_DEFAULT;
  normalized_variant.BindFlags = D3D11_BIND_RENDER_TARGET;
  normalized_variant.CPUAccessFlags = 0;
  normalized_variant.MiscFlags = 0;
  assert(ShouldReusePrivateCopyTexture(cached, normalized_variant));
}

void Nv12TextureCachePolicyRestrictsReuseToAsyncMatchingDevice() {
  const Nv12TextureCacheKey cached{{1920, 1080}, DXGI_FORMAT_NV12};
  const Nv12TextureCacheKey same{{1920, 1080}, DXGI_FORMAT_NV12};
  const Nv12TextureCacheKey resized{{2560, 1440}, DXGI_FORMAT_NV12};
  const Nv12TextureCacheKey reformatted{{1920, 1080}, DXGI_FORMAT_P010};

  assert(ShouldReuseNv12OutputTexture(true, cached, same, true));
  assert(!ShouldReuseNv12OutputTexture(true, cached, resized, true));
  assert(!ShouldReuseNv12OutputTexture(true, cached, reformatted, true));
  assert(!ShouldReuseNv12OutputTexture(true, cached, same, false));
  assert(!ShouldReuseNv12OutputTexture(false, cached, same, true));
}

void ResolutionLadderPrefersNativeFitAndMaintainsAspectRatio() {
  assert((SelectEncodeResolution({3200, 2136}, 3200) == Size{3200, 2136}));
  assert((SelectEncodeResolution({3840, 2160}, 3200) == Size{3200, 1800}));
  assert((SelectEncodeResolution({3840, 2160}, 2560) == Size{2560, 1440}));
  assert((SelectEncodeResolution({3201, 2137}, 3200) == Size{3200, 2136}));
  assert((SelectEncodeResolution({1920, 1080}, 1919) == Size{}));
}

void ResolutionLadderStepsDownWithoutUpscaling() {
  assert((NextLowerResolution({3200, 1800}, {3840, 2160}) == Size{2560, 1440}));
  assert((NextLowerResolution({2560, 1440}, {3840, 2160}) == Size{1920, 1080}));
  assert((NextLowerResolution({1920, 1080}, {3840, 2160}) == Size{}));
}

void AdaptiveResolutionControllerUsesPersistentBreachesAndStableUpgrade() {
  AdaptiveResolutionController controller;
  controller.Start(3200);
  controller.SetNativeSize({3840, 2160});
  const auto start = SteadyTime{} + std::chrono::seconds(1);

  ResolutionPolicyObservation breach{};
  breach.timestamp = start;
  breach.queue_age = std::chrono::milliseconds(51);
  assert(controller.Observe(breach).action == ResolutionPolicyAction::kNone);
  breach.timestamp += std::chrono::milliseconds(1);
  assert(controller.Observe(breach).action == ResolutionPolicyAction::kNone);
  breach.timestamp += std::chrono::milliseconds(1);
  const auto downgrade = controller.Observe(breach);
  assert(downgrade.action == ResolutionPolicyAction::kDowngrade);
  assert(downgrade.request_stream_reset);
  assert(downgrade.max_longest_edge == 2560);
  assert((downgrade.encode_size == Size{2560, 1440}));

  ResolutionPolicyObservation clean{};
  clean.timestamp = breach.timestamp + std::chrono::seconds(9);
  assert(controller.Observe(clean).action == ResolutionPolicyAction::kNone);
  clean.timestamp += std::chrono::seconds(1);
  const auto upgrade = controller.Observe(clean);
  assert(upgrade.action == ResolutionPolicyAction::kUpgrade);
  assert(upgrade.max_longest_edge == 3200);
  assert((upgrade.encode_size == Size{3200, 1800}));
}

void AdaptiveResolutionControllerNeverDropsBelowMinimumRung() {
  AdaptiveResolutionController controller;
  controller.Start(3200);
  controller.SetNativeSize({3840, 2160});
  auto timestamp = SteadyTime{} + std::chrono::seconds(1);
  ResolutionPolicyObservation breach{};
  breach.queue_age = std::chrono::milliseconds(51);
  for (int rung = 0; rung < 6; ++rung) {
    breach.timestamp = timestamp;
    timestamp += std::chrono::milliseconds(1);
    static_cast<void>(controller.Observe(breach));
  }
  assert(controller.current_max_longest_edge() == 1920);
  breach.timestamp = timestamp;
  static_cast<void>(controller.Observe(breach));
  assert(controller.current_max_longest_edge() == 1920);
}

void EncoderSelectionUsesDeterministicTieBreak() {
  H264EncoderCandidateDiagnostics current{};
  current.usable = true;
  current.name = L"AMD Hardware H.264";
  current.enumeration_index = 4;
  current.measured_latency = std::chrono::microseconds(100);
  current.adapter_scoped = true;

  H264EncoderCandidateDiagnostics faster = current;
  faster.name = L"NVIDIA Hardware H.264";
  faster.measured_latency = std::chrono::microseconds(99);
  assert(PreferH264EncoderCandidate(faster, current));

  H264EncoderCandidateDiagnostics same_latency = current;
  same_latency.name = L"A Hardware H.264";
  same_latency.enumeration_index = 1;
  assert(PreferH264EncoderCandidate(same_latency, current));
  assert(!PreferH264EncoderCandidate(current, same_latency));

  H264EncoderCandidateDiagnostics unusable = current;
  unusable.usable = false;
  assert(!PreferH264EncoderCandidate(unusable, current));
  assert(PreferH264EncoderCandidate(current, unusable));

  H264EncoderCandidateDiagnostics unscoped = faster;
  unscoped.adapter_scoped = false;
  assert(!PreferH264EncoderCandidate(unscoped, current));
  assert(PreferH264EncoderCandidate(current, unscoped));
}

void EncoderFormalStreamSetupRequiresTypesBeforeStart() {
  H264EncoderStreamSetupSequence sequence;
  assert(!sequence.complete());
  assert(!sequence.media_types_configured());
  assert(!sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetD3DManager));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kConfigureCodecApi));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetOutputType));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetInputType));
  assert(!sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.media_types_configured());
  assert(sequence.Advance(H264EncoderStreamSetupStep::kBeginStreaming));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.complete());
}

void OptionalCodecApiPropertyFailuresUseDocumentedDefaults() {
  assert(IsOptionalH264CodecApiPropertyFailure(E_INVALIDARG));
  assert(IsOptionalH264CodecApiPropertyFailure(E_NOTIMPL));
  assert(IsOptionalH264CodecApiPropertyFailure(E_NOINTERFACE));
  assert(IsOptionalH264CodecApiPropertyFailure(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)));
  assert(!IsOptionalH264CodecApiPropertyFailure(E_FAIL));
}

void EncoderOutputStreamChangesRenegotiateWithBoundedRetries() {
  assert(ClassifyH264EncoderOutputStatus(S_OK, 0, 0) ==
         H264EncoderOutputAction::kConsumeOutput);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_NEED_MORE_INPUT, 0, 0) ==
         H264EncoderOutputAction::kNeedMoreInput);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_STREAM_CHANGE,
             MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE, 0) ==
         H264EncoderOutputAction::kRenegotiateOutput);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_STREAM_CHANGE, MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE,
             kMaxH264EncoderOutputStreamChanges - 1) ==
         H264EncoderOutputAction::kRenegotiateOutput);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_STREAM_CHANGE, MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE,
             kMaxH264EncoderOutputStreamChanges) ==
         H264EncoderOutputAction::kFail);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_STREAM_CHANGE, 0, 0) ==
         H264EncoderOutputAction::kRetryOutput);
  assert(ClassifyH264EncoderOutputStatus(
             MF_E_TRANSFORM_STREAM_CHANGE,
             MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE, 0) ==
         H264EncoderOutputAction::kRetryOutput);
  assert(ClassifyH264EncoderOutputStatus(E_FAIL, 0, 0) ==
         H264EncoderOutputAction::kFail);

  assert(ShouldRetryEmptyH264EncoderOutput(false, 0));
  assert(ShouldRetryEmptyH264EncoderOutput(
      false, kMaxH264EncoderEmptyOutputRetries - 1));
  assert(!ShouldRetryEmptyH264EncoderOutput(
      false, kMaxH264EncoderEmptyOutputRetries));
  assert(!ShouldRetryEmptyH264EncoderOutput(true, 0));
}

void LatestFrameQueueReplacesOnlyPendingRawFrame() {
  LatestFrameQueue<std::string> queue;
  queue.Push("old");
  queue.Push("new");
  const auto value = queue.TryPop();
  assert(value.has_value() && *value == "new");
  assert(queue.replaced_count() == 1);
  assert(!queue.TryPop().has_value());
}

void LatestFrameQueuePreservesNewerFrameWhenRetryingAsyncInput() {
  LatestFrameQueue<std::string> queue;
  queue.Push("newer");
  assert(!queue.PushIfEmpty("older"));
  const auto newer = queue.TryPop();
  assert(newer.has_value() && *newer == "newer");
  assert(queue.PushIfEmpty("retry"));
  const auto retry = queue.TryPop();
  assert(retry.has_value() && *retry == "retry");
}

void VideoLoopPollingDecisionKeepsCaptureBehindEncoderPoll() {
  assert(DecideVideoLoopAfterInitialEncode(S_OK) ==
         VideoLoopAfterEncodeAction::kSendOutput);
  assert(DecideVideoLoopAfterInitialEncode(S_FALSE) ==
         VideoLoopAfterEncodeAction::kCapture);
  assert(DecideVideoLoopAfterInitialEncode(E_FAIL) ==
         VideoLoopAfterEncodeAction::kFail);
  assert(ShouldEncodeAfterCapture(S_OK));
  assert(!ShouldEncodeAfterCapture(S_FALSE));
  assert(!ShouldEncodeAfterCapture(HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
}

void VideoLoopSkipsDesktopCaptureWhileAsyncInputIsPending() {
  assert(!ShouldAttemptDesktopCapture(true));
  assert(ShouldAttemptDesktopCapture(false));
  assert(!HasInFlightEncodeState(false, false));
  assert(HasInFlightEncodeState(true, false));
  assert(HasInFlightEncodeState(false, true));
  assert(HasInFlightEncodeState(true, true));
  assert(ShouldYieldAfterPendingEncodeNoProgress(S_FALSE, S_FALSE, true));
  assert(!ShouldYieldAfterPendingEncodeNoProgress(S_FALSE, S_FALSE, false));
  assert(!ShouldYieldAfterPendingEncodeNoProgress(S_OK, S_FALSE, true));
  assert(!ShouldYieldAfterPendingEncodeNoProgress(S_FALSE, S_OK, true));
}

void BackpressureRequiresResetAndFreshIdr() {
  EncodedBackpressurePolicy policy;
  assert(policy.MaySendAccessUnit());
  assert(policy.OnTransportWouldBlock() == TransportAction::kRequestStreamReset);
  assert(!policy.MaySendAccessUnit());
  assert(policy.OnTransportWouldBlock() == TransportAction::kHold);
  assert(policy.reset_requests() == 1);
  policy.OnFreshIdrProduced();
  assert(policy.MaySendAccessUnit());
}

void AsyncMftRequiresNeedInputAndKeepsOnlyOneFrameInFlight() {
  AsyncMftStateMachine state;
  state.StartStream();
  assert(!state.CanAcceptInput());

  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(state.CanAcceptInput());
  assert(state.OnInputAccepted(17));
  assert(!state.CanAcceptInput());
  assert(state.HasPendingInput());

  // Hardware is allowed to ask early, but accepting another frame would make
  // the encode queue deeper than the latency contract permits.
  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(!state.CanAcceptInput());
}

void AsyncMftAssociatesOutputAndHandlesStaleOutputEvents() {
  AsyncMftStateMachine state;
  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(state.OnInputAccepted(23));
  state.OnEvent(AsyncMftEvent::kHaveOutput);
  assert(state.HasOutput());
  const auto sequence = state.OnOutputProduced();
  assert(sequence.has_value() && *sequence == 23);
  assert(!state.HasPendingInput());

  state.OnEvent(AsyncMftEvent::kHaveOutput);
  const auto stale_sequence = state.OnOutputProduced();
  assert(!stale_sequence.has_value());
}

void AsyncMftStreamChangeConsumesCurrentOutputEventAndKeepsPendingInput() {
  AsyncMftStateMachine state;
  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(state.OnInputAccepted(29));
  state.OnEvent(AsyncMftEvent::kHaveOutput);
  assert(state.HasOutput());

  // ProcessOutput reported STREAM_CHANGE and the caller renegotiated the
  // output type. That HaveOutput event is consumed without completing the
  // pending frame; a second event is required before ProcessOutput is legal.
  state.OnOutputUnavailable();
  assert(!state.HasOutput());
  assert(state.HasPendingInput());
  assert(state.pending_sequence().has_value() && *state.pending_sequence() == 29);

  state.OnEvent(AsyncMftEvent::kHaveOutput);
  const auto sequence = state.OnOutputProduced();
  assert(sequence.has_value() && *sequence == 29);
  assert(!state.HasPendingInput());
}

void AsyncMftDrainAndFailureSuppressFurtherInput() {
  AsyncMftStateMachine state;
  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  state.BeginDrain();
  assert(!state.CanAcceptInput());
  state.OnEvent(AsyncMftEvent::kEndOfStream);
  state.OnEvent(AsyncMftEvent::kDrainComplete);
  assert(state.end_of_stream());
  assert(state.drain_complete());

  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  state.MarkFatal();
  assert(state.faulted());
  assert(!state.CanAcceptInput());
  assert(state.TakeRestartRequest());
  assert(!state.TakeRestartRequest());
}

void PendingEncodeTelemetryCompletesDelayedOutputExactlyOnce() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::milliseconds(10);
  const auto started = acquired + std::chrono::milliseconds(4);
  assert(state.Begin({42, acquired, std::chrono::microseconds(4'000), started}));

  EncodedAccessUnit output{};
  output.sequence = 42;
  output.encoded_at = started + std::chrono::milliseconds(20);
  output.is_idr = true;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, output.encoded_at, &telemetry) ==
         EncodeTelemetryCompletion::kCompleted);
  assert(telemetry.sequence == 42);
  assert(output.acquired_at == acquired);
  assert(telemetry.acquired_at == acquired);
  assert(telemetry.encoded_at == output.encoded_at);
  assert(telemetry.conversion_duration == std::chrono::microseconds(4'000));
  assert(telemetry.encode_duration == std::chrono::milliseconds(20));
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(24));
  assert(telemetry.idr);
  assert(!state.has_pending());
  assert(state.Complete(output, output.encoded_at, &telemetry) ==
         EncodeTelemetryCompletion::kNoPending);
}

void EncodedAccessUnitCarriesSynchronousCaptureAndEncodeTimes() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::milliseconds(100);
  const auto started = acquired + std::chrono::milliseconds(2);
  const auto encoded = acquired + std::chrono::milliseconds(8);
  assert(state.Begin({7, acquired, std::chrono::milliseconds(2), started}));

  EncodedAccessUnit output{};
  output.sequence = 7;
  output.encoded_at = encoded;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, encoded, &telemetry) == EncodeTelemetryCompletion::kCompleted);
  assert(output.acquired_at == acquired);
  assert(output.encoded_at == encoded);
  assert(telemetry.acquired_at == acquired);
  assert(telemetry.encoded_at == encoded);
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(8));
}

void EncodedAccessUnitCarriesDelayedAsyncCaptureTimeBySequence() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::seconds(3);
  const auto started = acquired + std::chrono::milliseconds(3);
  const auto encoded = acquired + std::chrono::milliseconds(27);
  assert(state.Begin({91, acquired, std::chrono::milliseconds(3), started}));

  // The output arrives later but retains the MFT-assigned sequence. The
  // pending input timestamp must be restored onto the output before the app
  // serializes its VideoFrame.
  EncodedAccessUnit output{};
  output.sequence = 91;
  output.encoded_at = encoded;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, encoded, &telemetry) == EncodeTelemetryCompletion::kCompleted);
  assert(output.acquired_at == acquired);
  assert(output.encoded_at == encoded);
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(27));
}

void PendingEncodeTelemetryResetClearsWithoutCallbacks() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({7, SteadyTime{}, {}, SteadyTime{}}));
  state.Clear();
  EncodedAccessUnit output{};
  output.sequence = 7;
  EncodeTelemetry telemetry{};
  std::uint32_t on_encode = 0;
  std::uint32_t on_policy = 0;
  const auto result = state.Complete(output, SteadyTime{}, &telemetry);
  if (result == EncodeTelemetryCompletion::kCompleted) {
    ++on_encode;
    ++on_policy;
  }
  assert(result == EncodeTelemetryCompletion::kNoPending);
  assert(on_encode == 0 && on_policy == 0);
}

void PendingEncodeTelemetryRejectsMismatchedSequenceAndDoesNotAttribute() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({11, SteadyTime{}, {}, SteadyTime{}}));
  EncodedAccessUnit output{};
  output.sequence = 12;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, SteadyTime{}, &telemetry) ==
         EncodeTelemetryCompletion::kSequenceMismatch);
  assert(!state.has_pending());
}

void PendingEncodeTelemetryCompletionDrivesExactlyOneEncodeAndPolicyCallback() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({19, SteadyTime{}, {}, SteadyTime{}}));
  EncodedAccessUnit output{};
  output.sequence = 19;
  output.encoded_at = SteadyTime{};
  EncodeTelemetry telemetry{};
  std::uint32_t on_encode = 0;
  std::uint32_t on_policy = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (state.Complete(output, SteadyTime{}, &telemetry) ==
        EncodeTelemetryCompletion::kCompleted) {
      ++on_encode;
      ++on_policy;
    }
  }
  assert(on_encode == 1 && on_policy == 1);
}

void AnnexBIdrDetectionHandlesStartCodesAndMalformedInput() {
  const auto contains = [](std::initializer_list<std::uint8_t> bytes) {
    const std::vector<std::uint8_t> access_unit(bytes);
    return AnnexBAccessUnitContainsIdr(access_unit);
  };
  assert(contains({0x00, 0x00, 0x01, 0x65, 0x88}));
  assert(contains({0x00, 0x00, 0x00, 0x01, 0x65, 0x88}));

  const std::vector<std::uint8_t> parameter_sets_and_idr{
      0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
      0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
      0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  assert(AnnexBAccessUnitContainsIdr(parameter_sets_and_idr));

  assert(!contains({0x00, 0x00, 0x01, 0x41, 0x9a}));
  assert(!contains({}));
  assert(!contains({0x00, 0x00, 0x01}));
  assert(!contains({0x00, 0x00, 0x00, 0x01}));
  assert(!contains({0x00, 0x00, 0x02, 0x65, 0x88}));
  assert(!contains({0x00, 0x00, 0x00, 0x02, 0x65, 0x88}));
  assert(!contains({0x00, 0x00, 0x01, 0x85, 0x88}));
}

void ForceKeyFrameVariantUsesDocumentedUInt32Type() {
  const VARIANT value = MakeForceKeyFrameVariant();
  assert(value.vt == VT_UI4);
  assert(value.ulVal == 1u);
}

void MonitorIdParsingMatchesSerializedFormat() {
  MonitorId parsed{};
  assert(MonitorId::TryParse(L"-1:123:4", &parsed));
  assert(parsed.adapter_luid.HighPart == -1);
  assert(parsed.adapter_luid.LowPart == 123u);
  assert(parsed.target_id == 4u);

  assert(!MonitorId::TryParse(L"-1:123", &parsed));
  assert(!MonitorId::TryParse(L"-1:x:4", &parsed));
  assert(!MonitorId::TryParse(L"-1:123:x", &parsed));
}

}  // namespace pdb::video::test

#if defined(PDB_VIDEO_TEST_MAIN)
int main() {
  pdb::video::test::TransferredComReferencesAreAdoptedWithoutAddRef();
  pdb::video::test::OutputSampleDispositionDistinguishesCallerAndReplacement();
  pdb::video::test::InputSampleCachePolicyRequiresCompletedInputAndMatchingTexture();
  pdb::video::test::ResetEncodedAccessUnitRetainsByteCapacity();
  pdb::video::test::PrivateCopyTextureDescriptionSupportsVideoProcessorInputView();
  pdb::video::test::PrivateCopyTextureCachePolicyReusesOnlyMatchingNormalizedDescription();
  pdb::video::test::Nv12TextureCachePolicyRestrictsReuseToAsyncMatchingDevice();
  pdb::video::test::ResolutionLadderPrefersNativeFitAndMaintainsAspectRatio();
  pdb::video::test::ResolutionLadderStepsDownWithoutUpscaling();
  pdb::video::test::AdaptiveResolutionControllerUsesPersistentBreachesAndStableUpgrade();
  pdb::video::test::AdaptiveResolutionControllerNeverDropsBelowMinimumRung();
  pdb::video::test::EncoderSelectionUsesDeterministicTieBreak();
  pdb::video::test::EncoderFormalStreamSetupRequiresTypesBeforeStart();
  pdb::video::test::OptionalCodecApiPropertyFailuresUseDocumentedDefaults();
  pdb::video::test::EncoderOutputStreamChangesRenegotiateWithBoundedRetries();
  pdb::video::test::LatestFrameQueueReplacesOnlyPendingRawFrame();
  pdb::video::test::LatestFrameQueuePreservesNewerFrameWhenRetryingAsyncInput();
  pdb::video::test::VideoLoopPollingDecisionKeepsCaptureBehindEncoderPoll();
  pdb::video::test::VideoLoopSkipsDesktopCaptureWhileAsyncInputIsPending();
  pdb::video::test::BackpressureRequiresResetAndFreshIdr();
  pdb::video::test::AsyncMftRequiresNeedInputAndKeepsOnlyOneFrameInFlight();
  pdb::video::test::AsyncMftAssociatesOutputAndHandlesStaleOutputEvents();
  pdb::video::test::AsyncMftStreamChangeConsumesCurrentOutputEventAndKeepsPendingInput();
  pdb::video::test::AsyncMftDrainAndFailureSuppressFurtherInput();
  pdb::video::test::PendingEncodeTelemetryCompletesDelayedOutputExactlyOnce();
  pdb::video::test::EncodedAccessUnitCarriesSynchronousCaptureAndEncodeTimes();
  pdb::video::test::EncodedAccessUnitCarriesDelayedAsyncCaptureTimeBySequence();
  pdb::video::test::PendingEncodeTelemetryResetClearsWithoutCallbacks();
  pdb::video::test::PendingEncodeTelemetryRejectsMismatchedSequenceAndDoesNotAttribute();
  pdb::video::test::PendingEncodeTelemetryCompletionDrivesExactlyOneEncodeAndPolicyCallback();
  pdb::video::test::AnnexBIdrDetectionHandlesStartCodesAndMalformedInput();
  pdb::video::test::ForceKeyFrameVariantUsesDocumentedUInt32Type();
  pdb::video::test::MonitorIdParsingMatchesSerializedFormat();
}
#endif
