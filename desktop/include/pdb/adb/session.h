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
  // Called by the transport owner once a channel has passed authentication.
  // Only then does the host consider the app to be genuinely in a session, so
  // a launch that never actually connects cannot be mistaken for a live
  // session that the user ended.
  void NotifySessionEstablished() noexcept;
  // Asks the session to stop starting new attempts immediately, without doing
  // the (slow) device cleanup yet. Call this *before* joining the ADB thread so
  // a shutdown cannot relaunch the tablet client while threads are winding
  // down. Stop() still performs the cleanup afterwards.
  void BeginShutdown() noexcept;
  // Restores the tablet settings immediately, without waiting for a full
  // teardown. Used on exit so the tablet leaves landscape mode right away even
  // when the rest of the shutdown is blocked on sockets.
  void RestoreDeviceNow() noexcept;

  [[nodiscard]] SessionSnapshot Snapshot() const;
  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool ValidateSessionToken(
      std::span<const std::uint8_t> token) const noexcept;

 private:
  bool AttemptConnect();
  // Decides whether a new launch attempt is allowed right now. Implements the
  // auto-start policy: launch on plug-in, adopt a manually opened client, and
  // stay idle once the client process disappears.
  bool CanStartNewAttempt();
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
  // Auto-start bookkeeping. A "generation" is one continuous period during
  // which a single authorized device is present.
  bool device_was_present_ = false;
  bool had_session_this_generation_ = false;
  bool awaiting_replug_ = false;
  std::uint64_t device_generation_ = 0;
  // After a user-initiated stop, ignore the app coming back to the foreground
  // for a short while. MIUI restarts whitelisted apps on its own and brings
  // them to the front, which would otherwise undo "closing the app stops it".
  std::chrono::steady_clock::time_point suppress_adopt_until_{};
  // True once the tablet settings were handed back after the user closed the
  // app; cleared whenever a new session is prepared.
  bool device_restored_ = false;
  // When the last channel was lost. Used to give the Android process a grace
  // period to actually exit before we conclude the user closed the app.
  std::chrono::steady_clock::time_point channel_loss_at_{};
};

}  // namespace pdb::adb
