#include "pdb/app/input_adapter.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {

using namespace paddrawboard::protocol;

ClientHello Hello() {
  ClientHello hello;
  hello.capabilities = kCapabilityPressure | kCapabilityTilt | kCapabilityDistance |
                       kCapabilityTouch | kCapabilityButton1 | kCapabilityButton2 | kCapabilityButton3;
  hello.maxPenPressure = 8192;
  return hello;
}

InputSample Pen(std::uint8_t flags, std::uint32_t delta = 0) {
  InputSample sample;
  sample.timestampDeltaUs = delta;
  sample.pointerId = 7;
  sample.tool = ToolType::Pen;
  sample.contactFlags = flags;
  sample.x = 32768;
  sample.y = 16384;
  sample.pressure = 4096;
  sample.tiltX = 1000;
  sample.tiltY = -500;
  sample.distance = 123;
  sample.buttons = kButton1 | kButton3;
  return sample;
}

InputBatch Batch(InputSample sample) {
  InputBatch batch;
  batch.batchTimestampNs = 123;
  batch.samples.push_back(sample);
  return batch;
}

void TestPenPhasesAxesAndTimestamps() {
  pdb::app::InputBatchAdapter adapter;
  adapter.Configure(Hello(), 4);
  const auto received = std::chrono::steady_clock::now();
  pdb::input::InputFrame frame;
  std::string error;
  assert(adapter.Adapt(Batch(Pen(kInRange)), received, &frame, &error));
  assert(frame.pens.size() == 1);
  assert(frame.pens[0].phase == pdb::input::PointerPhase::kHover);
  assert(frame.pens[0].orientationEpoch == 4);
  assert(frame.pens[0].pressureAvailable);
  assert(frame.pens[0].tiltAvailable);
  assert(std::fabs(frame.pens[0].pressure - 0.5F) < 0.001F);
  assert(std::fabs(frame.pens[0].tiltXDegrees - 10.0F) < 0.001F);
  assert((frame.pens[0].buttons & pdb::input::kPenButtonPrimary) != 0);
  assert((frame.pens[0].buttons & pdb::input::kPenButtonTertiary) != 0);

  assert(adapter.Adapt(Batch(Pen(kInRange | kContact, 40)), received, &frame, &error));
  assert(frame.pens[0].phase == pdb::input::PointerPhase::kDown);
  const auto down_time = frame.pens[0].timestamp;
  assert(adapter.Adapt(Batch(Pen(kInRange | kContact, 80)), received, &frame, &error));
  assert(frame.pens[0].phase == pdb::input::PointerPhase::kMove);
  assert(frame.pens[0].timestamp >= down_time);
  assert(adapter.Adapt(Batch(Pen(kInRange, 120)), received, &frame, &error));
  assert(frame.pens[0].phase == pdb::input::PointerPhase::kUp);
}

void TestTouchCancelAndCapabilityDowngrade() {
  pdb::app::InputBatchAdapter adapter;
  adapter.Configure(Hello(), 1);
  InputSample touch;
  touch.pointerId = 2;
  touch.tool = ToolType::Touch;
  touch.contactFlags = kContact | kPrimary;
  touch.x = 1;
  touch.y = 2;
  touch.pressure = 4000;
  pdb::input::InputFrame frame;
  assert(adapter.Adapt(Batch(touch), std::chrono::steady_clock::now(), &frame));
  assert(frame.touches.size() == 1);
  assert(frame.touches[0].phase == pdb::input::PointerPhase::kDown);
  touch.contactFlags = kCancelled;
  assert(adapter.Adapt(Batch(touch), std::chrono::steady_clock::now(), &frame));
  assert(frame.touches[0].phase == pdb::input::PointerPhase::kCancel);

  adapter.SetCapabilities(kCapabilityTouch);
  adapter.SetOrientationEpoch(9);
  assert(adapter.Adapt(Batch(Pen(kInRange)), std::chrono::steady_clock::now(), &frame));
  assert(!frame.pens[0].pressureAvailable);
  assert(!frame.pens[0].tiltAvailable);
  assert(frame.pens[0].orientationEpoch == 9);
}

}  // namespace

int main() {
  TestPenPhasesAxesAndTimestamps();
  TestTouchCancelAndCapabilityDowngrade();
  std::cout << "pdb_app_input_adapter_tests: all tests passed\n";
  return 0;
}
