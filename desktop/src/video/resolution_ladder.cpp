#include "pdb/video/resolution_ladder.h"

#include <algorithm>

namespace pdb::video {
namespace {

Size ScaleToLongestEdge(Size input, std::uint32_t longest_edge) {
  if (!input.valid()) return {};
  const auto native_longest = std::max(input.width, input.height);
  if (native_longest <= longest_edge) {
    return {std::max(2u, input.width & ~1u), std::max(2u, input.height & ~1u)};
  }

  const auto scale = static_cast<double>(longest_edge) / native_longest;
  auto width = static_cast<std::uint32_t>(input.width * scale) & ~1u;
  auto height = static_cast<std::uint32_t>(input.height * scale) & ~1u;
  return {std::max(2u, width), std::max(2u, height)};
}

}  // namespace

Size SelectEncodeResolution(Size native, std::uint32_t max_longest_edge) {
  if (!native.valid() || max_longest_edge < 2) return {};
  const auto native_longest = std::max(native.width, native.height);
  if (native_longest <= max_longest_edge) return ScaleToLongestEdge(native, max_longest_edge);

  for (const auto rung : kLongestEdgeLadder) {
    if (rung <= max_longest_edge) return ScaleToLongestEdge(native, rung);
  }
  return {};
}

Size NextLowerResolution(Size current, Size native) {
  if (!current.valid() || !native.valid()) return {};
  const auto current_longest = std::max(current.width, current.height);
  for (const auto rung : kLongestEdgeLadder) {
    if (rung < current_longest) return ScaleToLongestEdge(native, rung);
  }
  return {};
}

void AdaptiveResolutionController::Start(std::uint32_t initial_max_longest_edge) noexcept {
  initial_max_longest_edge_ = ClampRung(initial_max_longest_edge);
  current_max_longest_edge_ = initial_max_longest_edge_;
  native_size_ = {};
  consecutive_breaches_ = 0;
  stable_since_ = {};
}

void AdaptiveResolutionController::SetNativeSize(Size native) noexcept {
  if (native == native_size_) return;
  native_size_ = native;
  // A monitor/rotation change starts a fresh policy epoch. Keep the configured
  // rung, but do not carry congestion history into a different geometry.
  consecutive_breaches_ = 0;
  stable_since_ = {};
}

std::uint32_t AdaptiveResolutionController::ClampRung(std::uint32_t requested) const noexcept {
  for (const auto rung : kLongestEdgeLadder) {
    if (requested == rung) return rung;
  }
  return 3200;
}

ResolutionPolicyDecision AdaptiveResolutionController::MakeDecision(
    ResolutionPolicyAction action, const ResolutionPolicyObservation& observation,
    bool breached) const noexcept {
  ResolutionPolicyDecision decision{};
  decision.action = action;
  decision.max_longest_edge = current_max_longest_edge_;
  decision.encode_size = current_encode_size();
  decision.release_budget_breached = breached;
  decision.request_stream_reset = action != ResolutionPolicyAction::kNone;
  decision.consecutive_breaches = consecutive_breaches_;
  if (stable_since_ != SteadyTime{} && observation.timestamp >= stable_since_) {
    decision.stable_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        observation.timestamp - stable_since_);
  }
  return decision;
}

ResolutionPolicyDecision AdaptiveResolutionController::Observe(
    const ResolutionPolicyObservation& observation) noexcept {
  const bool breached = observation.transport_blocked ||
                        observation.queue_age > kReleaseBudget ||
                        observation.capture_to_encode_duration > kReleaseBudget ||
                        observation.encode_duration > kEncodeBudget;
  if (stable_since_ == SteadyTime{}) stable_since_ = observation.timestamp;

  if (breached) {
    ++consecutive_breaches_;
    stable_since_ = observation.timestamp;
    if (consecutive_breaches_ >= kPersistentBreachSamples && current_max_longest_edge_ > 1920) {
      const auto current = current_max_longest_edge_;
      current_max_longest_edge_ = current == 3200 ? 2560 : 1920;
      consecutive_breaches_ = 0;
      stable_since_ = observation.timestamp;
      return MakeDecision(ResolutionPolicyAction::kDowngrade, observation, true);
    }
    return MakeDecision(ResolutionPolicyAction::kNone, observation, true);
  }

  consecutive_breaches_ = 0;
  if (current_max_longest_edge_ < initial_max_longest_edge_ &&
      observation.timestamp >= stable_since_ &&
      observation.timestamp - stable_since_ >= kStableUpgradeWindow) {
    current_max_longest_edge_ = current_max_longest_edge_ == 1920 ? 2560 : 3200;
    stable_since_ = observation.timestamp;
    return MakeDecision(ResolutionPolicyAction::kUpgrade, observation, false);
  }
  return MakeDecision(ResolutionPolicyAction::kNone, observation, false);
}

}  // namespace pdb::video
