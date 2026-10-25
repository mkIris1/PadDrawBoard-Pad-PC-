#include "pdb/app/input_adapter.h"

#include <algorithm>
#include <chrono>

namespace pdb::app {
namespace {

using paddrawboard::protocol::ContactFlag;
using paddrawboard::protocol::InputSample;
using paddrawboard::protocol::ToolType;

constexpr float kAxisScale = 1.0F / 65535.0F;

float NormalizedAxis(std::uint16_t value) noexcept {
  return static_cast<float>(value) * kAxisScale;
}

float NormalizedPressure(std::uint16_t value, std::uint16_t maximum) noexcept {
  return std::clamp(static_cast<float>(value) / static_cast<float>(std::max<std::uint16_t>(maximum, 1)),
                    0.0F, 1.0F);
}

bool HasFlag(const InputSample& sample, ContactFlag flag) noexcept {
  return (sample.contactFlags & static_cast<std::uint8_t>(flag)) != 0;
}

bool HasCapability(std::uint32_t capabilities, std::uint32_t capability) noexcept {
  return (capabilities & capability) != 0;
}

}  // namespace

void InputBatchAdapter::Configure(const paddrawboard::protocol::ClientHello& hello,
                                  std::uint64_t orientation_epoch) {
  capabilities_ = hello.capabilities;
  max_pressure_ = std::max<std::uint16_t>(hello.maxPenPressure, 1);
  orientation_epoch_ = orientation_epoch;
  Reset();
}

void InputBatchAdapter::SetCapabilities(std::uint32_t capabilities) noexcept {
  capabilities_ = capabilities;
}

void InputBatchAdapter::SetOrientationEpoch(std::uint64_t orientation_epoch) noexcept {
  orientation_epoch_ = orientation_epoch;
  contacts_.clear();
}

void InputBatchAdapter::Reset() noexcept {
  contacts_.clear();
  last_timestamp_ = {};
}

bool InputBatchAdapter::Adapt(const paddrawboard::protocol::InputBatch& batch,
                              std::chrono::steady_clock::time_point received_at,
                              input::InputFrame* output, std::string* error) {
  if (output == nullptr) {
    if (error != nullptr) *error = "null InputFrame destination";
    return false;
  }
  output->pens.clear();
  output->touches.clear();
  if (batch.samples.empty() || batch.samples.size() > paddrawboard::protocol::kMaxInputSamples) {
    if (error != nullptr) *error = "invalid InputBatch sample count";
    return false;
  }
  // The client clock is intentionally not assumed to share the host epoch.
  // Anchor the newest history sample to receive time while preserving all
  // device-provided deltas for hover/palm timing and pointer ordering.
  const auto latest_delta = std::chrono::microseconds(batch.samples.back().timestampDeltaUs);
  const auto batch_start = received_at - latest_delta;
  for (const InputSample& sample : batch.samples) {
    const bool cancelled = HasFlag(sample, ContactFlag::kCancelled);
    const bool contact = HasFlag(sample, ContactFlag::kContact);
    const bool in_range = HasFlag(sample, ContactFlag::kInRange);
    const std::uint32_t key = ContactKey(sample);
    const bool was_contact = contacts_.contains(key) && contacts_.at(key);
    input::PointerPhase phase = PhaseFor(sample, was_contact);
    auto timestamp = batch_start + std::chrono::microseconds(sample.timestampDeltaUs);
    if (timestamp < last_timestamp_) timestamp = last_timestamp_;
    last_timestamp_ = timestamp;

    if (cancelled || !contact) {
      contacts_.erase(key);
    } else {
      contacts_[key] = true;
    }

    if (sample.tool == ToolType::Touch) {
      // A detached, never-seen touch cannot be a meaningful injection event.
      if (!contact && !cancelled && !was_contact) continue;
      input::TouchSample touch;
      touch.pointerId = sample.pointerId;
      touch.orientationEpoch = orientation_epoch_;
      touch.phase = phase;
      touch.position = {NormalizedAxis(sample.x), NormalizedAxis(sample.y)};
      touch.pressure = NormalizedPressure(sample.pressure, max_pressure_);
      touch.pressureAvailable = HasCapability(capabilities_, paddrawboard::protocol::kCapabilityPressure);
      touch.timestamp = timestamp;
      output->touches.push_back(std::move(touch));
      continue;
    }

    input::PenSample pen;
    pen.pointerId = sample.pointerId;
    pen.orientationEpoch = orientation_epoch_;
    pen.phase = phase;
    pen.position = {NormalizedAxis(sample.x), NormalizedAxis(sample.y)};
    pen.pressure = NormalizedPressure(sample.pressure, max_pressure_);
    pen.pressureAvailable = HasCapability(capabilities_, paddrawboard::protocol::kCapabilityPressure);
    pen.tiltXDegrees = static_cast<float>(sample.tiltX) / 100.0F;
    pen.tiltYDegrees = static_cast<float>(sample.tiltY) / 100.0F;
    pen.tiltAvailable = HasCapability(capabilities_, paddrawboard::protocol::kCapabilityTilt);
    pen.distance = sample.distance == paddrawboard::protocol::kDistanceUnavailable
                       ? 0.0F : NormalizedAxis(sample.distance);
    pen.inRange = in_range;
    pen.eraser = sample.tool == ToolType::Eraser;
    if ((sample.buttons & paddrawboard::protocol::kButton1) != 0) pen.buttons |= input::kPenButtonPrimary;
    if ((sample.buttons & paddrawboard::protocol::kButton2) != 0) pen.buttons |= input::kPenButtonSecondary;
    if ((sample.buttons & paddrawboard::protocol::kButton3) != 0) pen.buttons |= input::kPenButtonTertiary;
    pen.timestamp = timestamp;
    output->pens.push_back(std::move(pen));
  }
  return true;
}

input::PointerPhase InputBatchAdapter::PhaseFor(const InputSample& sample,
                                                bool was_contact) const noexcept {
  if (HasFlag(sample, ContactFlag::kCancelled)) return input::PointerPhase::kCancel;
  if (HasFlag(sample, ContactFlag::kContact)) {
    return was_contact ? input::PointerPhase::kMove : input::PointerPhase::kDown;
  }
  return was_contact ? input::PointerPhase::kUp : input::PointerPhase::kHover;
}

std::uint32_t InputBatchAdapter::ContactKey(const InputSample& sample) noexcept {
  return (static_cast<std::uint32_t>(sample.tool) << 16U) | sample.pointerId;
}

}  // namespace pdb::app
