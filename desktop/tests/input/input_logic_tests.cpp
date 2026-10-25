#include "pdb/input/mapper.h"
#include "pdb/input/palm_guard.h"
#include "pdb/input/touch_grouping.h"

#include <cassert>
#include <chrono>

namespace pdb::input::tests {
void RunCoordinateMapperTests() {
  CoordinateMapper mapper;
  MapperConfig config;
  config.tabletActiveRect = {0.1F, 0.2F, 0.9F, 0.8F};
  config.monitor = {100, 200, 2000, 1000};
  config.contentAspectRatio = 1.0F;
  config.orientationEpoch = 7;
  assert(mapper.Configure(config));
  assert((mapper.ContentViewport() == PixelRect{600, 200, 1000, 1000}));
  assert((mapper.Map({0.1F, 0.2F}, 7) == MappedPoint{600, 200}));
  assert((mapper.Map({0.9F, 0.8F}, 7) == MappedPoint{1599, 1199}));
  assert(!mapper.Map({0.05F, 0.2F}, 7).has_value());
  assert(!mapper.Map({0.1F, 0.2F}, 8).has_value());

  config.rotation = Rotation::k90;
  assert(mapper.Configure(config));
  assert((mapper.Map({0.1F, 0.2F}, 7) == MappedPoint{1599, 200}));
}

void RunPalmGuardTests() {
  using namespace std::chrono_literals;
  PalmGuard guard;
  const auto start = std::chrono::steady_clock::time_point{};
  assert(guard.AllowsTouch(start));
  guard.ObservePen(true, start);
  assert(!guard.AllowsTouch(start));
  guard.ObservePen(false, start + 10ms);
  assert(guard.AllowsTouch(start + 10ms));

  // Hover updates remain non-contact and cannot re-latch suppression.
  guard.ObservePen(false, start + 20ms);
  assert(guard.AllowsTouch(start + 20ms));

  PalmGuard delayed{150ms};
  delayed.ObservePen(true, start);
  delayed.ObservePen(false, start + 10ms);
  assert(!delayed.AllowsTouch(start + 159ms));
  assert(delayed.AllowsTouch(start + 160ms));
}

void RunTouchGroupingTests() {
  using namespace std::chrono_literals;
  const auto start = std::chrono::steady_clock::time_point{};
  const std::vector<TouchSample> history{
      {.pointerId = 1, .timestamp = start},
      {.pointerId = 2, .timestamp = start},
      {.pointerId = 1, .timestamp = start + 1ms},
      {.pointerId = 1, .timestamp = start + 2ms},
  };
  assert(NextTouchInjectionGroupEnd(history, 0) == 2);
  assert(NextTouchInjectionGroupEnd(history, 2) == 3);
  assert(NextTouchInjectionGroupEnd(history, 3) == 4);

  const std::vector<TouchSample> same_tick_updates{
      {.pointerId = 1, .timestamp = start},
      {.pointerId = 1, .timestamp = start},
  };
  assert(NextTouchInjectionGroupEnd(same_tick_updates, 0) == 1);
  assert(NextTouchInjectionGroupEnd(same_tick_updates, 1) == 2);
}

}  // namespace pdb::input::tests

#ifdef PDB_INPUT_TEST_MAIN
int main() {
  pdb::input::tests::RunCoordinateMapperTests();
  pdb::input::tests::RunPalmGuardTests();
  pdb::input::tests::RunTouchGroupingTests();
}
#endif
