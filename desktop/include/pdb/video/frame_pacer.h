#pragma once

#include "pdb/video/types.h"

#include <algorithm>
#include <thread>

namespace pdb::video {

// A process-local high-resolution timer avoids the ~15ms rounding of ordinary
// Windows sleeps without changing the system timer resolution. Called only
// between completed frames; normal waits are at most one 60Hz frame interval.
class FramePacer final {
 public:
  FramePacer() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
  ~FramePacer() { if (timer_) CloseHandle(timer_); }
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;

  void WaitUntil(SteadyTime deadline) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= SteadyTime::duration::zero()) return;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(1,
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      (void)WaitForSingleObject(timer_, INFINITE);
    } else {
      std::this_thread::sleep_until(deadline);
    }
  }

 private:
  HANDLE timer_{};
};

}  // namespace pdb::video
