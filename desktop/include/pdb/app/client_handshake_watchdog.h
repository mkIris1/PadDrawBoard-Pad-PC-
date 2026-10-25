#pragma once

#include <chrono>
#include <optional>

namespace pdb::app {

// AdbSession::connected() means the launch command completed, not that the
// authenticated socket handshake succeeded. Relaunch when that distinction
// persists, for example after Android restores a launcher Intent without the
// one-time session token.
class ClientHandshakeWatchdog final {
 public:
  explicit ClientHandshakeWatchdog(
      std::chrono::milliseconds timeout = std::chrono::seconds{5})
      : timeout_(timeout) {}

  bool Observe(bool adb_connected, bool client_session_active,
               std::chrono::steady_clock::time_point now) noexcept {
    if (!adb_connected || client_session_active) {
      waiting_since_.reset();
      return false;
    }
    if (!waiting_since_.has_value()) {
      waiting_since_ = now;
      return false;
    }
    if (now - *waiting_since_ < timeout_) return false;
    waiting_since_ = now;
    return true;
  }

 private:
  std::chrono::milliseconds timeout_;
  std::optional<std::chrono::steady_clock::time_point> waiting_since_;
};

}  // namespace pdb::app
