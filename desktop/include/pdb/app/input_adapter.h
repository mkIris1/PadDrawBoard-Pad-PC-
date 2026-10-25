#pragma once

#include "paddraw_protocol.hpp"
#include "pdb/input/domain.h"

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace pdb::app {

// Converts the wire representation to the input subsystem's host-domain
// samples.  It derives edges from contact state because the compact protocol
// intentionally carries flags rather than a separate phase byte.
class InputBatchAdapter final {
 public:
  void Configure(const paddrawboard::protocol::ClientHello& hello,
                 std::uint64_t orientation_epoch);
  void SetCapabilities(std::uint32_t capabilities) noexcept;
  void SetOrientationEpoch(std::uint64_t orientation_epoch) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool Adapt(const paddrawboard::protocol::InputBatch& batch,
                           std::chrono::steady_clock::time_point received_at,
                           input::InputFrame* output,
                           std::string* error = nullptr);

 private:
  [[nodiscard]] input::PointerPhase PhaseFor(
      const paddrawboard::protocol::InputSample& sample, bool was_contact) const noexcept;
  [[nodiscard]] static std::uint32_t ContactKey(
      const paddrawboard::protocol::InputSample& sample) noexcept;

  std::uint32_t capabilities_{};
  std::uint16_t max_pressure_{1};
  std::uint64_t orientation_epoch_{};
  std::unordered_map<std::uint32_t, bool> contacts_;
  std::chrono::steady_clock::time_point last_timestamp_{};
};

}  // namespace pdb::app
