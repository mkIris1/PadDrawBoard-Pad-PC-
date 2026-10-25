#pragma once

#include "pdb/input/domain.h"

#include <chrono>

namespace pdb::input {

// Suppresses fingers only while the pen tip is down. Hover must not latch touch
// suppression because some Android styluses never report a range-exit event.
class PalmGuard final {
 public:
  explicit PalmGuard(std::chrono::milliseconds releaseDelay = std::chrono::milliseconds{0});

  void ObservePen(bool tipDown, std::chrono::steady_clock::time_point now) noexcept;
  [[nodiscard]] bool AllowsTouch(std::chrono::steady_clock::time_point now) const noexcept;
  void Reset() noexcept;

 private:
  std::chrono::milliseconds releaseDelay_;
  std::chrono::steady_clock::time_point lastPenExit_{};
  bool penTipDown_{};
  bool seenPen_{};
};

}  // namespace pdb::input
