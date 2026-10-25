#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pdb::input {

struct NormalizedPoint {
  float x{};
  float y{};
};

// Coordinates are normalized to the Android view, not to a protocol packet.
struct NormalizedRect {
  float left{};
  float top{};
  float right{1.0F};
  float bottom{1.0F};

  [[nodiscard]] bool IsValid() const noexcept {
    return left >= 0.0F && top >= 0.0F && right <= 1.0F && bottom <= 1.0F &&
           right > left && bottom > top;
  }
};

struct PixelRect {
  std::int32_t left{};
  std::int32_t top{};
  std::int32_t width{};
  std::int32_t height{};

  [[nodiscard]] bool IsValid() const noexcept { return width > 0 && height > 0; }
  bool operator==(const PixelRect&) const = default;
};

enum class Rotation : std::uint16_t { k0 = 0, k90 = 90, k180 = 180, k270 = 270 };
enum class PointerPhase : std::uint8_t { kHover, kDown, kMove, kUp, kCancel };

enum PenButton : std::uint32_t {
  kPenButtonNone = 0,
  kPenButtonPrimary = 1u << 0,
  kPenButtonSecondary = 1u << 1,
  kPenButtonTertiary = 1u << 2,
};

struct PenSample {
  std::uint32_t pointerId{1};
  std::uint64_t orientationEpoch{};
  PointerPhase phase{PointerPhase::kHover};
  NormalizedPoint position{};
  float pressure{};       // [0, 1], ignored when unavailable.
  float tiltXDegrees{};   // [-90, 90]
  float tiltYDegrees{};   // [-90, 90]
  float distance{};       // hardware-defined normalized distance, diagnostic only.
  bool inRange{};
  bool pressureAvailable{};
  bool tiltAvailable{};
  bool eraser{};
  std::uint32_t buttons{kPenButtonNone};
  std::chrono::steady_clock::time_point timestamp{};
};

struct TouchSample {
  std::uint32_t pointerId{};
  std::uint64_t orientationEpoch{};
  PointerPhase phase{PointerPhase::kMove};
  NormalizedPoint position{};
  float contactWidth{0.01F};
  float contactHeight{0.01F};
  float pressure{};
  bool pressureAvailable{};
  std::chrono::steady_clock::time_point timestamp{};
};

struct InputFrame {
  std::vector<PenSample> pens;
  std::vector<TouchSample> touches;
};

}  // namespace pdb::input
