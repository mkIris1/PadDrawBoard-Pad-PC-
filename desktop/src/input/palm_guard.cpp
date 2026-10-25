#include "pdb/input/palm_guard.h"

namespace pdb::input {
PalmGuard::PalmGuard(std::chrono::milliseconds releaseDelay) : releaseDelay_(releaseDelay) {}

void PalmGuard::ObservePen(bool tipDown, std::chrono::steady_clock::time_point now) noexcept {
  if (penTipDown_ && !tipDown) lastPenExit_ = now;
  penTipDown_ = tipDown;
  seenPen_ = true;
}

bool PalmGuard::AllowsTouch(std::chrono::steady_clock::time_point now) const noexcept {
  return !penTipDown_ && (!seenPen_ || now - lastPenExit_ >= releaseDelay_);
}

void PalmGuard::Reset() noexcept {
  penTipDown_ = false;
  seenPen_ = false;
  lastPenExit_ = {};
}
}  // namespace pdb::input
