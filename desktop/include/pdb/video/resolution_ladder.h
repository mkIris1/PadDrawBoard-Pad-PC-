#pragma once

#include "pdb/video/types.h"

#include <array>
#include <chrono>

namespace pdb::video {

inline constexpr std::array<std::uint32_t, 3> kLongestEdgeLadder{3200, 2560, 1920};

// Returns the native size when it fits, otherwise the first supported ladder
// rung that fits. Dimensions are even because NV12 4:2:0 requires it.
[[nodiscard]] Size SelectEncodeResolution(Size native, std::uint32_t max_longest_edge);
[[nodiscard]] Size NextLowerResolution(Size current, Size native);

enum class ResolutionPolicyAction {
  kNone,
  kDowngrade,
  kUpgrade,
};

struct ResolutionPolicyObservation {
  SteadyTime timestamp{};
  std::chrono::microseconds queue_age{};
  std::chrono::microseconds encode_duration{};
  std::chrono::microseconds capture_to_encode_duration{};
  bool transport_blocked{};
};

struct ResolutionPolicyDecision {
  ResolutionPolicyAction action{ResolutionPolicyAction::kNone};
  std::uint32_t max_longest_edge{};
  Size encode_size{};
  bool release_budget_breached{};
  bool request_stream_reset{};
  std::uint32_t consecutive_breaches{};
  std::chrono::milliseconds stable_duration{};
};

// Pure runtime policy. It changes rungs only after persistent budget breaches,
// and requires ten seconds of clean observations before upgrading. The
// returned decision is deliberately side-effect free; the video pipeline owns
// encoder reset/reconfiguration and telemetry emission.
class AdaptiveResolutionController final {
 public:
  static constexpr std::chrono::microseconds kReleaseBudget{50'000};
  static constexpr std::chrono::microseconds kEncodeBudget{16'666};
  static constexpr std::chrono::seconds kStableUpgradeWindow{10};
  static constexpr std::uint32_t kPersistentBreachSamples{3};

  void Start(std::uint32_t initial_max_longest_edge = 3200) noexcept;
  void SetNativeSize(Size native) noexcept;
  [[nodiscard]] ResolutionPolicyDecision Observe(const ResolutionPolicyObservation& observation) noexcept;
  [[nodiscard]] std::uint32_t current_max_longest_edge() const noexcept {
    return current_max_longest_edge_;
  }
  [[nodiscard]] Size current_encode_size() const noexcept {
    return SelectEncodeResolution(native_size_, current_max_longest_edge_);
  }
  [[nodiscard]] Size native_size() const noexcept { return native_size_; }
  [[nodiscard]] std::uint32_t consecutive_breaches() const noexcept {
    return consecutive_breaches_;
  }

 private:
  [[nodiscard]] ResolutionPolicyDecision MakeDecision(ResolutionPolicyAction action,
                                                      const ResolutionPolicyObservation& observation,
                                                      bool breached) const noexcept;
  [[nodiscard]] std::uint32_t ClampRung(std::uint32_t requested) const noexcept;

  Size native_size_{};
  std::uint32_t current_max_longest_edge_{3200};
  std::uint32_t initial_max_longest_edge_{3200};
  std::uint32_t consecutive_breaches_{};
  SteadyTime stable_since_{};
};

}  // namespace pdb::video
