#include "pdb/input/synthetic_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <sdkddkver.h>
// These APIs were introduced in Windows 10 RS5. PadDrawBoard targets Windows 11.
#if NTDDI_VERSION < NTDDI_WIN10_RS5
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_RS5
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace pdb::input {
namespace {
constexpr std::uint32_t kMaxContacts = 10;
using CreateSyntheticPointerDeviceFn = HSYNTHETICPOINTERDEVICE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
using InjectSyntheticPointerInputFn = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
using DestroySyntheticPointerDeviceFn = VOID(WINAPI*)(HSYNTHETICPOINTERDEVICE);

LONG ToPressure(float pressure) noexcept {
  return static_cast<LONG>(std::lround(std::clamp(pressure, 0.0F, 1.0F) * 1024.0F));
}

LONG ToTilt(float tilt) noexcept {
  return static_cast<LONG>(std::lround(std::clamp(tilt, -90.0F, 90.0F)));
}

POINTER_FLAGS PhaseFlags(PointerPhase phase, bool inRange) noexcept {
  switch (phase) {
    case PointerPhase::kHover: return (inRange ? POINTER_FLAG_INRANGE : POINTER_FLAG_NONE) | POINTER_FLAG_UPDATE;
    case PointerPhase::kDown: return POINTER_FLAG_NEW | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
    case PointerPhase::kMove: return (inRange ? POINTER_FLAG_INRANGE : POINTER_FLAG_NONE) | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
    case PointerPhase::kUp: return (inRange ? POINTER_FLAG_INRANGE : POINTER_FLAG_NONE) | POINTER_FLAG_UP;
    case PointerPhase::kCancel: return POINTER_FLAG_CANCELED | POINTER_FLAG_UP;
  }
  return POINTER_FLAG_NONE;
}

POINTER_INFO PointerInfo(POINTER_INPUT_TYPE type, std::uint32_t pointerId, PointerPhase phase,
                         bool inRange, MappedPoint point) noexcept {
  POINTER_INFO info{};
  info.pointerType = type;
  info.pointerId = pointerId;
  info.pointerFlags = PhaseFlags(phase, inRange);
  info.ptPixelLocation = POINT{point.x, point.y};
  info.ptHimetricLocation = POINT{point.x, point.y};
  return info;
}
}  // namespace

SyntheticPointerInjector::SyntheticPointerInjector() {
  const HMODULE user32 = LoadLibraryW(L"user32.dll");
  if (user32 == nullptr) return;
  user32Module_ = user32;
  createDevice_ = reinterpret_cast<void*>(GetProcAddress(user32, "CreateSyntheticPointerDevice"));
  injectInput_ = reinterpret_cast<void*>(GetProcAddress(user32, "InjectSyntheticPointerInput"));
  destroyDevice_ = reinterpret_cast<void*>(GetProcAddress(user32, "DestroySyntheticPointerDevice"));
  if (createDevice_ == nullptr || injectInput_ == nullptr || destroyDevice_ == nullptr) return;
  const auto createDevice = reinterpret_cast<CreateSyntheticPointerDeviceFn>(createDevice_);
  penDevice_ = createDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
  touchDevice_ = createDevice(PT_TOUCH, kMaxContacts, POINTER_FEEDBACK_DEFAULT);
}

SyntheticPointerInjector::~SyntheticPointerInjector() {
  ReleaseAll();
  if (destroyDevice_ != nullptr) {
    const auto destroyDevice = reinterpret_cast<DestroySyntheticPointerDeviceFn>(destroyDevice_);
    if (penDevice_ != nullptr) destroyDevice(static_cast<HSYNTHETICPOINTERDEVICE>(penDevice_));
    if (touchDevice_ != nullptr) destroyDevice(static_cast<HSYNTHETICPOINTERDEVICE>(touchDevice_));
  }
  if (user32Module_ != nullptr) FreeLibrary(static_cast<HMODULE>(user32Module_));
}

bool SyntheticPointerInjector::IsAvailable() const noexcept {
  return penDevice_ != nullptr && touchDevice_ != nullptr && injectInput_ != nullptr;
}

bool SyntheticPointerInjector::InjectPen(const PenSample& sample, MappedPoint point, bool forceEraser) noexcept {
  if (penDevice_ == nullptr || injectInput_ == nullptr) return false;
  POINTER_TYPE_INFO info{};
  info.type = PT_PEN;
  info.penInfo.pointerInfo = PointerInfo(PT_PEN, sample.pointerId, sample.phase, sample.inRange, point);
  info.penInfo.penMask = 0;
  if (sample.pressureAvailable) {
    info.penInfo.penMask |= PEN_MASK_PRESSURE;
    info.penInfo.pressure = ToPressure(sample.pressure);
  }
  if (sample.tiltAvailable) {
    info.penInfo.penMask |= PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    info.penInfo.tiltX = ToTilt(sample.tiltXDegrees);
    info.penInfo.tiltY = ToTilt(sample.tiltYDegrees);
  }
  if ((sample.buttons & kPenButtonPrimary) != 0) info.penInfo.penFlags |= PEN_FLAG_BARREL;
  if (sample.eraser || forceEraser) info.penInfo.penFlags |= PEN_FLAG_ERASER;
  const auto injectInput = reinterpret_cast<InjectSyntheticPointerInputFn>(injectInput_);
  const bool ok = injectInput(static_cast<HSYNTHETICPOINTERDEVICE>(penDevice_), &info, 1) != FALSE;
  penActive_ = ok && sample.phase != PointerPhase::kUp && sample.phase != PointerPhase::kCancel;
  penPointerId_ = sample.pointerId;
  return ok;
}

bool SyntheticPointerInjector::InjectTouches(std::span<const TouchSample> samples,
                                             std::span<const MappedPoint> points) noexcept {
  if (touchDevice_ == nullptr || injectInput_ == nullptr || samples.size() != points.size() || samples.size() > kMaxContacts) return false;
  std::array<POINTER_TYPE_INFO, kMaxContacts> infos{};
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const auto& sample = samples[i];
    if (sample.pointerId == 0 || sample.pointerId > kMaxContacts) return false;
    auto& info = infos[i];
    info.type = PT_TOUCH;
    info.touchInfo.pointerInfo = PointerInfo(PT_TOUCH, sample.pointerId, sample.phase, true, points[i]);
    info.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA;
    const LONG halfWidth = std::max<LONG>(1, static_cast<LONG>(std::lround(sample.contactWidth * 500.0F)));
    const LONG halfHeight = std::max<LONG>(1, static_cast<LONG>(std::lround(sample.contactHeight * 500.0F)));
    info.touchInfo.rcContact = RECT{points[i].x - halfWidth, points[i].y - halfHeight,
                                    points[i].x + halfWidth, points[i].y + halfHeight};
    if (sample.pressureAvailable) {
      info.touchInfo.touchMask |= TOUCH_MASK_PRESSURE;
      info.touchInfo.pressure = ToPressure(sample.pressure);
    }
    const std::uint32_t bit = 1u << (sample.pointerId - 1);
    touchPoints_[sample.pointerId - 1] = points[i];
    if (sample.phase == PointerPhase::kUp || sample.phase == PointerPhase::kCancel) touchActiveMask_ &= ~bit;
    else touchActiveMask_ |= bit;
  }
  const auto injectInput = reinterpret_cast<InjectSyntheticPointerInputFn>(injectInput_);
  return samples.empty() || injectInput(static_cast<HSYNTHETICPOINTERDEVICE>(touchDevice_),
                                        infos.data(), static_cast<UINT32>(samples.size())) != FALSE;
}

void SyntheticPointerInjector::ReleaseAll() noexcept {
  if (penActive_ && penDevice_ != nullptr) {
    PenSample cancel;
    cancel.pointerId = penPointerId_;
    cancel.phase = PointerPhase::kCancel;
    (void)InjectPen(cancel, MappedPoint{});
  }
  ReleaseTouches();
  penActive_ = false;
}

void SyntheticPointerInjector::ReleaseTouches() noexcept {
  if (touchActiveMask_ != 0 && touchDevice_ != nullptr && injectInput_ != nullptr) {
    std::array<POINTER_TYPE_INFO, kMaxContacts> infos{};
    UINT32 count{};
    for (std::uint32_t index = 0; index < kMaxContacts; ++index) {
      if ((touchActiveMask_ & (1u << index)) == 0) continue;
      POINTER_TYPE_INFO info{};
      info.type = PT_TOUCH;
      info.touchInfo.pointerInfo = PointerInfo(PT_TOUCH, index + 1, PointerPhase::kCancel, false, touchPoints_[index]);
      infos[count++] = info;
    }
    const auto injectInput = reinterpret_cast<InjectSyntheticPointerInputFn>(injectInput_);
    if (count != 0) (void)injectInput(static_cast<HSYNTHETICPOINTERDEVICE>(touchDevice_), infos.data(), count);
  }
  touchActiveMask_ = 0;
}
}  // namespace pdb::input
