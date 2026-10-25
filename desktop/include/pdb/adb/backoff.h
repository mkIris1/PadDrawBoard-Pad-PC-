#pragma once

#include <chrono>
#include <cstddef>

namespace pdb::adb {

struct BackoffPolicy {
  std::chrono::milliseconds initial_delay{250};
  std::chrono::milliseconds maximum_delay{8'000};
  std::size_t multiplier = 2;
};

class ReconnectBackoff {
 public:
  explicit ReconnectBackoff(BackoffPolicy policy = {});

  [[nodiscard]] std::chrono::milliseconds delay_for_attempt(
      std::size_t zero_based_attempt) const noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }
  [[nodiscard]] std::chrono::milliseconds next_delay() const noexcept;
  void record_failure() noexcept;

 private:
  BackoffPolicy policy_;
  std::size_t attempts_ = 0;
};

}  // namespace pdb::adb
