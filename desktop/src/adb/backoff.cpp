#include "pdb/adb/backoff.h"

#include <algorithm>
#include <limits>

namespace pdb::adb {

ReconnectBackoff::ReconnectBackoff(BackoffPolicy policy) : policy_(policy) {
  if (policy_.multiplier == 0) {
    policy_.multiplier = 1;
  }
  if (policy_.maximum_delay < policy_.initial_delay) {
    policy_.maximum_delay = policy_.initial_delay;
  }
}

std::chrono::milliseconds ReconnectBackoff::delay_for_attempt(
    std::size_t zero_based_attempt) const noexcept {
  auto delay = policy_.initial_delay;
  for (std::size_t index = 0; index < zero_based_attempt; ++index) {
    const auto current = delay.count();
    const auto maximum = policy_.maximum_delay.count();
    if (current >= maximum || current > maximum / static_cast<long long>(policy_.multiplier)) {
      return policy_.maximum_delay;
    }
    delay = std::chrono::milliseconds(current *
                                      static_cast<long long>(policy_.multiplier));
  }
  return std::min(delay, policy_.maximum_delay);
}

void ReconnectBackoff::reset() noexcept { attempts_ = 0; }

std::chrono::milliseconds ReconnectBackoff::next_delay() const noexcept {
  return delay_for_attempt(attempts_ == 0 ? 0 : attempts_ - 1);
}

void ReconnectBackoff::record_failure() noexcept {
  if (attempts_ != std::numeric_limits<std::size_t>::max()) {
    ++attempts_;
  }
}

}  // namespace pdb::adb
