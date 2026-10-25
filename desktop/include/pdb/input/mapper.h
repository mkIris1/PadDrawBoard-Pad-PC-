#pragma once

#include "pdb/input/domain.h"

#include <cstdint>
#include <optional>

namespace pdb::input {

struct MapperConfig {
  NormalizedRect tabletActiveRect{};
  PixelRect monitor{};
  // Aspect ratio of the video/content canvas. Zero uses the selected monitor.
  float contentAspectRatio{};
  Rotation rotation{Rotation::k0};
  std::uint64_t orientationEpoch{};
};

struct MappedPoint {
  std::int32_t x{};
  std::int32_t y{};
  bool operator==(const MappedPoint&) const = default;
};

class CoordinateMapper final {
 public:
  [[nodiscard]] bool Configure(const MapperConfig& config) noexcept;
  [[nodiscard]] std::optional<MappedPoint> Map(NormalizedPoint point,
                                                std::uint64_t orientationEpoch) const noexcept;
  [[nodiscard]] PixelRect ContentViewport() const noexcept;
  [[nodiscard]] std::uint64_t OrientationEpoch() const noexcept;

 private:
  MapperConfig config_{};
  PixelRect viewport_{};
  bool configured_{};
};

}  // namespace pdb::input
