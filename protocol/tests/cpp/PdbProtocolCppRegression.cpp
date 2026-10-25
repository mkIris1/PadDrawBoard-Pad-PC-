#include "paddraw_protocol.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace paddrawboard::protocol;

int main() {
  const ClockSyncComplete complete{1'000'000'000ULL, 1'000'001'000ULL, 1'000'002'000ULL, 1'000'005'000ULL};
  const Frame frame{{0, 12, MessageType::Control}, Control{complete}};
  const std::vector<std::uint8_t> encoded = encodeFrame(frame);
  const std::vector<std::uint8_t> expected = {
      0x50, 0x44, 0x42, 0x31, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0c, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x08, 0x00, 0x20, 0x00,
      0x00, 0xca, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00, 0xe8, 0xcd, 0x9a, 0x3b,
      0x00, 0x00, 0x00, 0x00, 0xd0, 0xd1, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00,
      0x88, 0xdd, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00};
  assert(encoded == expected);

  const auto decoded = decodeFrame(encoded);
  assert(std::holds_alternative<Frame>(decoded));
  const auto& decodedControl = std::get<Control>(std::get<Frame>(decoded).payload);
  assert(std::get<ClockSyncComplete>(decodedControl).deviceReceiveNs == complete.deviceReceiveNs);

  auto malformed = encoded;
  malformed[22] = 0x18;
  malformed[23] = 0x00;
  assert(std::holds_alternative<ParseError>(decodeFrame(malformed)));
  return 0;
}
