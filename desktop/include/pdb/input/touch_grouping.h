#pragma once

#include "pdb/input/domain.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace pdb::input {

// One Windows injection describes one instant and may contain each pointer
// only once. Android MotionEvent history is a time-ordered flat sequence.
inline std::size_t NextTouchInjectionGroupEnd(
    std::span<const TouchSample> samples, std::size_t begin) noexcept {
  if (begin >= samples.size()) return samples.size();
  constexpr std::size_t kMaxContacts = 10;
  const auto timestamp = samples[begin].timestamp;
  std::size_t end = begin;
  while (end < samples.size() && end - begin < kMaxContacts &&
         samples[end].timestamp == timestamp) {
    const auto group_begin = samples.begin() + static_cast<std::ptrdiff_t>(begin);
    const auto group_end = samples.begin() + static_cast<std::ptrdiff_t>(end);
    const auto duplicate = std::find_if(group_begin, group_end, [&](const TouchSample& prior) {
      return prior.pointerId == samples[end].pointerId;
    });
    if (duplicate != group_end) break;
    ++end;
  }
  return end;
}

}  // namespace pdb::input
