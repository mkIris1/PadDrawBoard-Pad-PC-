#pragma once

#include "pdb/input/domain.h"
#include "pdb/input/mapper.h"

#include <cstdint>
#include <array>
#include <span>

namespace pdb::input {

class SyntheticPointerInjector final {
 public:
  SyntheticPointerInjector();
  ~SyntheticPointerInjector();
  SyntheticPointerInjector(const SyntheticPointerInjector&) = delete;
  SyntheticPointerInjector& operator=(const SyntheticPointerInjector&) = delete;

  [[nodiscard]] bool IsAvailable() const noexcept;
  [[nodiscard]] bool InjectPen(const PenSample& sample, MappedPoint point,
                               bool forceEraser = false) noexcept;
  [[nodiscard]] bool InjectTouches(std::span<const TouchSample> samples,
                                   std::span<const MappedPoint> points) noexcept;
  void ReleaseTouches() noexcept;
  void ReleaseAll() noexcept;

 private:
  void* penDevice_{};
  void* touchDevice_{};
  void* user32Module_{};
  void* createDevice_{};
  void* injectInput_{};
  void* destroyDevice_{};
  bool penActive_{};
  std::uint32_t penPointerId_{1};
  std::uint32_t touchActiveMask_{};
  std::array<MappedPoint, 10> touchPoints_{};
};

}  // namespace pdb::input
