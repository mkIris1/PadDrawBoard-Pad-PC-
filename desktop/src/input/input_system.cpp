#include "pdb/input/input_system.h"
#include "pdb/input/touch_grouping.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <span>

namespace pdb::input {
namespace {
constexpr std::array<std::uint32_t, 3> kButtonBits{
    kPenButtonPrimary, kPenButtonSecondary, kPenButtonTertiary};

DWORD MouseFlag(MouseButton button, bool down) noexcept {
  switch (button) {
    case MouseButton::kLeft: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    case MouseButton::kMiddle: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    case MouseButton::kRight: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    case MouseButton::kX1:
    case MouseButton::kX2: return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
  }
  return 0;
}

void SendKey(std::uint16_t key, bool down) noexcept {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = key;
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  (void)SendInput(1, &input, sizeof(input));
}

void SendAction(const Action& action, bool down) noexcept {
  if (action.kind == ActionKind::kDisabled || action.kind == ActionKind::kEraser) return;
  if (down) {
    for (const auto key : action.virtualKeys) SendKey(key, true);
  }
  if (action.kind == ActionKind::kMouseButton) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MouseFlag(action.mouseButton, down);
    if (action.mouseButton == MouseButton::kX1) input.mi.mouseData = XBUTTON1;
    if (action.mouseButton == MouseButton::kX2) input.mi.mouseData = XBUTTON2;
    (void)SendInput(1, &input, sizeof(input));
  } else if (action.kind == ActionKind::kKeyChord) {
    // The keys below form the chord; no additional action is needed on press.
  }
  if (!down) {
    for (auto iterator = action.virtualKeys.rbegin(); iterator != action.virtualKeys.rend(); ++iterator) {
      SendKey(*iterator, false);
    }
  }
}
}  // namespace

InputSystem::InputSystem() {
  profiles_.SetProfiles({ProfileResolver::BlenderProfile()});
}

bool InputSystem::Configure(const MapperConfig& mapperConfig) {
  std::scoped_lock lock(mutex_);
  ReleaseProfileActions();
  injector_.ReleaseAll();
  palmGuard_.Reset();
  touchesSuppressed_ = false;
  return mapper_.Configure(mapperConfig);
}

void InputSystem::SetPalmRejectionEnabled(bool enabled) noexcept {
  std::scoped_lock lock(mutex_);
  palmRejectionEnabled_ = enabled;
  if (!enabled) touchesSuppressed_ = false;
}

void InputSystem::SetProfiles(std::vector<ApplicationProfile> profiles) {
  std::scoped_lock lock(mutex_);
  ReleaseProfileActions();
  profiles_.SetProfiles(std::move(profiles));
}

bool InputSystem::Process(const InputFrame& frame) {
  std::scoped_lock lock(mutex_);
  bool success = true;
  for (const PenSample& pen : frame.pens) {
    const auto point = mapper_.Map(pen.position, pen.orientationEpoch);
    if (!point.has_value()) {
      ReleaseProfileActions();
      injector_.ReleaseAll();
      palmGuard_.Reset();
      touchesSuppressed_ = false;
      return false;  // stale orientation and black-bar input both cancel safely.
    }
    const bool pen_tip_down = pen.phase == PointerPhase::kDown ||
                              pen.phase == PointerPhase::kMove;
    palmGuard_.ObservePen(pen_tip_down, pen.timestamp);
    ApplyProfileButtons(pen);
    if (palmRejectionEnabled_ && !palmGuard_.AllowsTouch(pen.timestamp)) {
      if (!touchesSuppressed_) injector_.ReleaseTouches();
      touchesSuppressed_ = true;
    }
    success = injector_.InjectPen(pen, *point, effectiveEraser_) && success;
  }

  std::size_t begin = 0;
  while (begin < frame.touches.size()) {
    const std::size_t end = NextTouchInjectionGroupEnd(frame.touches, begin);
    std::vector<TouchSample> allowedSamples;
    std::vector<MappedPoint> points;
    allowedSamples.reserve(end - begin);
    points.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
      const TouchSample& touch = frame.touches[index];
      const auto point = mapper_.Map(touch.position, touch.orientationEpoch);
      if (!point.has_value()) {
        injector_.ReleaseTouches();
        return false;
      }
      if (palmRejectionEnabled_ && !palmGuard_.AllowsTouch(touch.timestamp)) {
        touchesSuppressed_ = true;
        continue;
      }
      touchesSuppressed_ = false;
      allowedSamples.push_back(touch);
      points.push_back(*point);
    }
    if (!allowedSamples.empty()) {
      success = injector_.InjectTouches(allowedSamples, points) && success;
    }
    begin = end;
  }
  return success;
}

void InputSystem::OnDisconnect() noexcept {
  std::scoped_lock lock(mutex_);
  ReleaseProfileActions();
  injector_.ReleaseAll();
  palmGuard_.Reset();
  touchesSuppressed_ = false;
}

void InputSystem::OnOrientationChange(const MapperConfig& mapperConfig) {
  std::scoped_lock lock(mutex_);
  ReleaseProfileActions();
  injector_.ReleaseAll();
  palmGuard_.Reset();
  touchesSuppressed_ = false;
  (void)mapper_.Configure(mapperConfig);
}

bool InputSystem::IsInjectionAvailable() const noexcept {
  std::scoped_lock lock(mutex_);
  return injector_.IsAvailable();
}

void InputSystem::ApplyProfileButtons(const PenSample& sample) {
  const ApplicationProfile& profile = profiles_.ResolveForeground();
  effectiveEraser_ = sample.eraser;
  for (std::size_t index = 0; index < kButtonBits.size(); ++index) {
    const bool nowPressed = (sample.buttons & kButtonBits[index]) != 0;
    if (nowPressed == profileButtons_[index]) {
      if (nowPressed && pressedActions_[index].kind == ActionKind::kEraser) effectiveEraser_ = true;
      continue;
    }
    if (profileButtons_[index]) SendAction(pressedActions_[index], false);
    profileButtons_[index] = nowPressed;
    if (nowPressed) {
      pressedActions_[index] = profile.buttons[index];
      SendAction(pressedActions_[index], true);
      if (pressedActions_[index].kind == ActionKind::kEraser) effectiveEraser_ = true;
    } else {
      pressedActions_[index] = Action{};
    }
  }
}

void InputSystem::ReleaseProfileActions() noexcept {
  for (std::size_t index = 0; index < profileButtons_.size(); ++index) {
    if (profileButtons_[index]) SendAction(pressedActions_[index], false);
    profileButtons_[index] = false;
    pressedActions_[index] = Action{};
  }
  effectiveEraser_ = false;
}
}  // namespace pdb::input
