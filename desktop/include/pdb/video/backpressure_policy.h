#pragma once

#include <cstdint>

namespace pdb::video {

enum class TransportAction { kSend, kHold, kRequestStreamReset };

// Unlike raw capture frames, AVC access units cannot be selectively thrown away.
// Once the transport signals congestion, the caller must drain/reset transport
// and request an IDR before it sends another access unit.
class EncodedBackpressurePolicy final {
 public:
  [[nodiscard]] TransportAction OnTransportWouldBlock();
  [[nodiscard]] bool MaySendAccessUnit() const noexcept { return !reset_pending_; }
  void OnStreamResetStarted() noexcept { reset_pending_ = true; }
  void OnFreshIdrProduced() noexcept { reset_pending_ = false; }
  [[nodiscard]] std::uint64_t reset_requests() const noexcept { return reset_requests_; }

 private:
  bool reset_pending_{};
  std::uint64_t reset_requests_{};
};

}  // namespace pdb::video
