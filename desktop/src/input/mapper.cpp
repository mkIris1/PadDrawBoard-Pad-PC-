#include "pdb/input/mapper.h"

#include <algorithm>
#include <cmath>

namespace pdb::input {
namespace {
constexpr float kEpsilon = 0.0001F;

float Clamp(float value, float low, float high) noexcept { return std::clamp(value, low, high); }
}  // namespace

bool CoordinateMapper::Configure(const MapperConfig& config) noexcept {
  if (!config.tabletActiveRect.IsValid() || !config.monitor.IsValid() ||
      (config.contentAspectRatio < 0.0F)) {
    viewport_ = {};
    configured_ = false;
    return false;
  }
  const float monitorAspect = static_cast<float>(config.monitor.width) / config.monitor.height;
  const float contentAspect = config.contentAspectRatio > kEpsilon ? config.contentAspectRatio : monitorAspect;
  if (contentAspect <= kEpsilon) {
    viewport_ = {};
    configured_ = false;
    return false;
  }
  PixelRect viewport = config.monitor;
  if (contentAspect > monitorAspect) {
    viewport.height = static_cast<std::int32_t>(std::lround(config.monitor.width / contentAspect));
    viewport.top += (config.monitor.height - viewport.height) / 2;
  } else if (contentAspect < monitorAspect) {
    viewport.width = static_cast<std::int32_t>(std::lround(config.monitor.height * contentAspect));
    viewport.left += (config.monitor.width - viewport.width) / 2;
  }
  config_ = config;
  viewport_ = viewport;
  configured_ = viewport_.IsValid();
  return configured_;
}

std::optional<MappedPoint> CoordinateMapper::Map(NormalizedPoint point,
                                                  std::uint64_t orientationEpoch) const noexcept {
  if (!configured_ || orientationEpoch != config_.orientationEpoch ||
      point.x < config_.tabletActiveRect.left || point.x > config_.tabletActiveRect.right ||
      point.y < config_.tabletActiveRect.top || point.y > config_.tabletActiveRect.bottom) {
    return std::nullopt;
  }
  float x = (point.x - config_.tabletActiveRect.left) /
            (config_.tabletActiveRect.right - config_.tabletActiveRect.left);
  float y = (point.y - config_.tabletActiveRect.top) /
            (config_.tabletActiveRect.bottom - config_.tabletActiveRect.top);
  switch (config_.rotation) {
    case Rotation::k0: break;
    case Rotation::k90: { const float oldX = x; x = 1.0F - y; y = oldX; break; }
    case Rotation::k180: x = 1.0F - x; y = 1.0F - y; break;
    case Rotation::k270: { const float oldX = x; x = y; y = 1.0F - oldX; break; }
  }
  return MappedPoint{viewport_.left + static_cast<std::int32_t>(std::lround(Clamp(x, 0.0F, 1.0F) * (viewport_.width - 1))),
                     viewport_.top + static_cast<std::int32_t>(std::lround(Clamp(y, 0.0F, 1.0F) * (viewport_.height - 1)))};
}

PixelRect CoordinateMapper::ContentViewport() const noexcept { return viewport_; }
std::uint64_t CoordinateMapper::OrientationEpoch() const noexcept { return config_.orientationEpoch; }
}  // namespace pdb::input
