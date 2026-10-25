#include "pdb/adb/session.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <utility>

namespace pdb::adb {

std::optional<SessionToken> GenerateSessionToken() {
  SessionToken token{};
  using BCryptGenRandomFn = LONG(WINAPI*)(void*, unsigned char*, unsigned long,
                                          unsigned long);
  HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
  if (bcrypt == nullptr) return std::nullopt;
  const FARPROC raw_generator = GetProcAddress(bcrypt, "BCryptGenRandom");
  static_assert(sizeof(raw_generator) == sizeof(BCryptGenRandomFn));
  const auto generator = std::bit_cast<BCryptGenRandomFn>(raw_generator);
  constexpr unsigned long kUseSystemPreferredRng = 0x00000002UL;
  const LONG result = generator == nullptr
      ? static_cast<LONG>(-1)
      : generator(nullptr, token.data(), static_cast<unsigned long>(token.size()),
                  kUseSystemPreferredRng);
  FreeLibrary(bcrypt);
  if (result != 0) {
    return std::nullopt;
  }
  return token;
}

std::chrono::steady_clock::time_point SteadyClock::Now() const {
  return std::chrono::steady_clock::now();
}

AdbSession::AdbSession(AdbClient& client, IClock& clock, SessionOptions options)
    : client_(client),
      clock_(clock),
      options_(std::move(options)),
      backoff_(options_.backoff) {}

AdbSession::~AdbSession() { Stop(); }

bool AdbSession::Start() {
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state == SessionState::Connected ||
        snapshot_.state == SessionState::Starting ||
        snapshot_.state == SessionState::Stopping) {
      return snapshot_.state == SessionState::Connected;
    }
    snapshot_.state = SessionState::Starting;
    authentication_armed_ = false;
    session_token_.reset();
    stop_source_ = std::stop_source{};
  }
  return AttemptConnect();
}

void AdbSession::Tick() {
  bool perform_health_check = false;
  std::string health_serial;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_) {
      return;
    }
    if (snapshot_.state == SessionState::Connected) {
      const auto now = clock_.Now();
      if (now < last_health_check_ + options_.health_check_interval) return;
      last_health_check_ = now;
      health_serial = snapshot_.serial;
      perform_health_check = true;
    } else {
      if (snapshot_.state != SessionState::Reconnecting ||
          clock_.Now() < snapshot_.next_retry) {
        return;
      }
      snapshot_.state = SessionState::Starting;
      authentication_armed_ = false;
      session_token_.reset();
      stop_source_ = std::stop_source{};
    }
  }
  if (perform_health_check) {
    const DeviceListing listing = client_.SelectSingleAuthorizedDevice();
    if (listing.status != DeviceSelectionStatus::Authorized ||
        !listing.selected.has_value() || listing.selected->serial != health_serial) {
      NotifyChannelLoss("ADB health check failed; reconnecting authorized device");
    }
    return;
  }
  AttemptConnect();
}

void AdbSession::Stop() noexcept {
  std::string serial;
  bool should_cleanup = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ && snapshot_.state == SessionState::Stopped) return;
    shutdown_requested_ = true;
    snapshot_.state = SessionState::Stopping;
    authentication_armed_ = false;
    session_token_.reset();
    serial = snapshot_.serial;
    should_cleanup = !serial.empty();
    stop_source_.request_stop();
  }
  if (should_cleanup) CleanupRemote(options_.stop_android_client_on_shutdown);
  {
    std::scoped_lock lock(mutex_);
    snapshot_.state = SessionState::Stopped;
    snapshot_.serial.clear();
    snapshot_.next_retry = {};
    last_health_check_ = {};
  }
}

void AdbSession::NotifyChannelLoss(std::string reason) noexcept {
  std::string serial;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state == SessionState::Stopped) return;
    serial = snapshot_.serial;
    stop_source_.request_stop();
    authentication_armed_ = false;
    session_token_.reset();
    backoff_.record_failure();
    snapshot_.state = SessionState::Reconnecting;
    snapshot_.last_error = std::move(reason);
    snapshot_.reconnect_attempt = backoff_.attempts();
    snapshot_.next_retry = clock_.Now() + backoff_.next_delay();
  }
  if (!serial.empty()) CleanupRemote(true);
}

SessionSnapshot AdbSession::Snapshot() const {
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

bool AdbSession::connected() const {
  std::scoped_lock lock(mutex_);
  return snapshot_.state == SessionState::Connected;
}

bool AdbSession::ValidateSessionToken(
    std::span<const std::uint8_t> token) const noexcept {
  std::scoped_lock lock(mutex_);
  const bool launch_is_in_progress = snapshot_.state == SessionState::Starting &&
                                     authentication_armed_;
  const bool session_is_connected = snapshot_.state == SessionState::Connected &&
                                    authentication_armed_;
  if ((!launch_is_in_progress && !session_is_connected) ||
      !session_token_.has_value() ||
      token.size() != kSessionTokenBytes) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < kSessionTokenBytes; ++index) {
    difference = static_cast<std::uint8_t>(
        difference | (token[index] ^ (*session_token_)[index]));
  }
  return difference == 0;
}

bool AdbSession::AttemptConnect() {
  const std::stop_token token = stop_source_.get_token();
  const DeviceListing listing = client_.SelectSingleAuthorizedDevice(token);
  if (listing.status != DeviceSelectionStatus::Authorized ||
      !listing.selected.has_value()) {
    std::string error = listing.diagnostics;
    if (error.empty()) {
      switch (listing.status) {
        case DeviceSelectionStatus::NoDevices: error = "no ADB device"; break;
        case DeviceSelectionStatus::Unauthorized: error = "ADB device unauthorized"; break;
        case DeviceSelectionStatus::Offline: error = "ADB device offline"; break;
        case DeviceSelectionStatus::MultipleDevices: error = "multiple ADB devices"; break;
        case DeviceSelectionStatus::MixedStates: error = "ADB device is not ready"; break;
        case DeviceSelectionStatus::UnknownState: error = "unknown ADB device state"; break;
        case DeviceSelectionStatus::CommandFailed: error = "ADB device listing failed"; break;
        case DeviceSelectionStatus::Authorized: break;
      }
    }
    ScheduleReconnect(std::move(error));
    return false;
  }
  {
    std::scoped_lock lock(mutex_);
    snapshot_.serial = listing.selected->serial;
  }
  const std::string serial = listing.selected->serial;
  const ProcessResult reverse = client_.InstallReverseMappings(
      serial, options_.ports, token);
  if (!reverse.succeeded()) {
    ScheduleReconnect(reverse.stderr_text.empty() ? "ADB reverse setup failed"
                                                   : reverse.stderr_text);
    return false;
  }
  const auto generated_token = GenerateSessionToken();
  if (!generated_token) {
    ScheduleReconnect("cryptographic session token generation failed");
    return false;
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state != SessionState::Starting) {
      return false;
    }
    session_token_ = *generated_token;
    authentication_armed_ = true;
  }
  const ProcessResult launch = client_.LaunchClient(
      serial, SessionTokenToHex(*generated_token), token);
  if (!launch.succeeded()) {
    ScheduleReconnect(launch.stderr_text.empty() ? "Android client launch failed"
                                                  : launch.stderr_text);
    return false;
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state != SessionState::Starting ||
        !authentication_armed_) {
      return false;
    }
    snapshot_.state = SessionState::Connected;
    snapshot_.last_error.clear();
    snapshot_.next_retry = {};
    snapshot_.reconnect_attempt = 0;
    backoff_.reset();
    last_health_check_ = clock_.Now();
  }
  return true;
}

void AdbSession::ScheduleReconnect(std::string error) {
  std::scoped_lock lock(mutex_);
  authentication_armed_ = false;
  session_token_.reset();
  if (shutdown_requested_) {
    snapshot_.state = SessionState::Stopped;
    return;
  }
  backoff_.record_failure();
  snapshot_.state = SessionState::Reconnecting;
  snapshot_.last_error = std::move(error);
  snapshot_.reconnect_attempt = backoff_.attempts();
  snapshot_.next_retry = clock_.Now() + backoff_.next_delay();
}

void AdbSession::CleanupRemote(bool stop_android_client) noexcept {
  const std::string serial = Snapshot().serial;
  if (serial.empty()) return;
  const std::stop_token no_cancel;
  if (stop_android_client) {
    (void)client_.StopClient(serial, no_cancel);
  }
  (void)client_.RemoveReverseMappings(serial, options_.ports, no_cancel);
}

}  // namespace pdb::adb
