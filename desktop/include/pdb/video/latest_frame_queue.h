#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace pdb::video {

// A deliberately tiny queue for latency-sensitive stages. Replacing its one
// pending item is safe for raw frames; encoded H.264 uses BackpressurePolicy.
template <typename T>
class LatestFrameQueue final {
 public:
  void Push(T value) {
    std::scoped_lock lock(mutex_);
    if (item_.has_value()) {
      ++replaced_count_;
    }
    item_.emplace(std::move(value));
  }

  // Used when an asynchronous hardware stage has not yet advertised capacity.
  // Keep the already-captured frame only when no newer capture won the race.
  [[nodiscard]] bool PushIfEmpty(T value) {
    std::scoped_lock lock(mutex_);
    if (item_.has_value()) return false;
    item_.emplace(std::move(value));
    return true;
  }

  [[nodiscard]] std::optional<T> TryPop() {
    std::scoped_lock lock(mutex_);
    if (!item_) return std::nullopt;
    std::optional<T> result{std::move(item_)};
    item_.reset();
    return result;
  }

  [[nodiscard]] bool empty() const {
    std::scoped_lock lock(mutex_);
    return !item_.has_value();
  }

  [[nodiscard]] std::uint64_t replaced_count() const {
    std::scoped_lock lock(mutex_);
    return replaced_count_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<T> item_;
  std::uint64_t replaced_count_{};
};

}  // namespace pdb::video
