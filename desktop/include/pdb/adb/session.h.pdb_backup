#pragma once

#include "pdb/adb/backoff.h"
#include "pdb/adb/client.h"

#include <chrono>
#include <mutex>
#include <stop_token>
#include <span>
#include <string>

namespace pdb::adb {

class IClock {
 public:
  virtual ~IClock() = default;
  [[nodiscard]] virtual std::chrono::steady_clock::time_point Now() const = 0;
};

class SteadyClock final : public IClock {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point Now() const override;
};

struct SessionOptions {
  SessionPorts ports;
  BackoffPolicy backoff;
  bool stop_android_client_on_shutdown = true;
  std::chrono::milliseconds health_check_interval{1'000};
};

class AdbSession final {
 public:
  AdbSession(AdbClient& client, IClock& clock, SessionOptions options = {});
  ~AdbSession();

  AdbSession(const AdbSession&) = delete;
  AdbSession& operator=(const AdbSession&) = delete;

  // Start performs one bounded connection attempt. Call Tick periodically to
  // perform retries without blocking the host application's main loop.
  bool Start();
  void Tick();
  void Stop() noexcept;
  // Called by the transport owner when any authenticated channel is lost.
  // This invalidates the current token, removes reverse mappings, and puts
  // the session into bounded-backoff reconnect without relaunching on a
  // clean desktop shutdown.
  void NotifyChannelLoss(std::string reason) noexcept;

  [[nodiscard]] SessionSnapshot Snapshot() const;
  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool ValidateSessionToken(
      std::span<const std::uint8_t> token) const noexcept;

 private:
  bool AttemptConnect();
  void ScheduleReconnect(std::string error);
  void CleanupRemote(bool stop_android_client) noexcept;

  AdbClient& client_;
  IClock& clock_;
  SessionOptions options_;
  ReconnectBackoff backoff_;
  mutable std::mutex mutex_;
  SessionSnapshot snapshot_;
  std::stop_source stop_source_;
  bool shutdown_requested_ = false;
  std::optional<SessionToken> session_token_;
  // Authentication is enabled only after the token for the current launch
  // attempt has been published, and is revoked on every terminal or retry
  // transition. This lets Android authenticate while am start is blocked
  // without accepting a token from an earlier session.
  bool authentication_armed_ = false;
  std::chrono::steady_clock::time_point last_health_check_{};
};

}  // namespace pdb::adb
