#include "pdb/adb/session.h"

#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

namespace {

// DSH-SESSIONLOG: append-only trace of the connection state machine. Costs one
// small line per decision and is the only practical way to diagnose a headless
// host (the tray tooltip is not reachable from a test harness).
void SessionTrace(const std::string& line) {
  static std::ofstream trace = [] {
    wchar_t appdata[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= static_cast<DWORD>(MAX_PATH)) return std::ofstream{};
    std::wstring path(appdata);
    path += L"\\PadDrawBoard\\session.log";
    return std::ofstream(path.c_str(), std::ios::app);
  }();
  if (!trace) return;
  SYSTEMTIME now{};
  GetLocalTime(&now);
  trace << now.wHour << ':' << now.wMinute << ':' << now.wSecond << '.'
        << now.wMilliseconds << ' ' << line << '\n';
  trace.flush();
}

// How often to poll the tablet while waiting for the user to reopen the app.
constexpr std::chrono::seconds kClientPollInterval{2};
// How long the client process must stay absent before we treat the end of a
// live session as a user-initiated close. Without this, "am force-stop" races
// with the next retry: the process is still visible for a moment and the host
// would relaunch the app the user just closed.
constexpr std::chrono::milliseconds kClientGoneGrace{3'000};
// How long to ignore the client reappearing after the user stopped it. MIUI
// revives whitelisted apps and focuses them, so without this the host would
// happily resume a session the user just ended.
constexpr std::chrono::seconds kUserStopCooldown{20};
}  // namespace

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
  // Auto-start policy: only arm a new launch when the device is present and the
  // client either is not running yet or was opened manually.
  if (!CanStartNewAttempt()) return;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state != SessionState::Reconnecting) {
      return;
    }
    snapshot_.state = SessionState::Starting;
    authentication_armed_ = false;
    session_token_.reset();
    stop_source_ = std::stop_source{};
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
  if (should_cleanup) {
    // Hand the tablet back in its own configuration before letting go: the
    // session only borrowed landscape + a pinned-awake panel.
    SessionTrace("restore: returning tablet settings to defaults");
    (void)client_.RestoreDevice(serial);
    CleanupRemote(options_.stop_android_client_on_shutdown);
  }
  {
    std::scoped_lock lock(mutex_);
    snapshot_.state = SessionState::Stopped;
    snapshot_.serial.clear();
    snapshot_.next_retry = {};
    last_health_check_ = {};
  }
}

void AdbSession::RestoreDeviceNow() noexcept {
  std::string serial;
  {
    std::scoped_lock lock(mutex_);
    serial = snapshot_.serial;
    if (device_restored_) return;
    device_restored_ = true;
  }
  if (serial.empty()) return;
  const std::stop_token no_cancel;
  SessionTrace("restore: early restore requested at exit");
  // Order matters: revert the tablet's settings *and* close its app first. Both
  // are single adb calls (~0.3 s each) and both are what the user actually sees
  // on exit. The remaining teardown (sockets, thread joins) can block for tens
  // of seconds, so it must not run ahead of these two.
  (void)client_.RestoreDevice(serial, no_cancel);
  (void)client_.StopClient(serial, no_cancel);
  SessionTrace("restore: tablet client stopped at exit");
}

void AdbSession::BeginShutdown() noexcept {
  {
    std::scoped_lock lock(mutex_);
    shutdown_requested_ = true;
    authentication_armed_ = false;
    session_token_.reset();
  }
  stop_source_.request_stop();
  SessionTrace("shutdown: no further launch attempts");
}

void AdbSession::NotifySessionEstablished() noexcept {
  SessionTrace("established: authenticated channel accepted");
  std::scoped_lock lock(mutex_);
  if (shutdown_requested_) return;
  had_session_this_generation_ = true;
  channel_loss_at_ = {};
  awaiting_replug_ = false;
  backoff_.reset();
}

void AdbSession::NotifyChannelLoss(std::string reason) noexcept {
  std::string serial;
  bool was_connected = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_requested_ || snapshot_.state == SessionState::Stopped) return;
    was_connected = snapshot_.state == SessionState::Connected;
    SessionTrace("channel loss: " + reason + " was_connected=" + std::to_string(was_connected ? 1 : 0));
    serial = snapshot_.serial;
    stop_source_.request_stop();
    authentication_armed_ = false;
    session_token_.reset();
    backoff_.record_failure();
    snapshot_.state = SessionState::Reconnecting;
    snapshot_.last_error = std::move(reason);
    snapshot_.reconnect_attempt = backoff_.attempts();
    snapshot_.next_retry = clock_.Now() + backoff_.next_delay();
    // Only a loss that ends a *live* session starts the "did the user close the
    // app?" grace window. Losses that happen while merely starting (for
    // example repeated handshake-watchdog timeouts) must not refresh it,
    // otherwise the window never expires and the host stops retrying.
    if (was_connected) channel_loss_at_ = clock_.Now();
  }
  // Deliberately not force-stopping the Android client here: the next attempt
  // uses "am start -S" (which force-stops) and keeping the process alive lets
  // the host distinguish a user-initiated close from a transient fault.
  if (!serial.empty()) CleanupRemote(false);

  // Plan 1, evaluated at the moment the session dies: if the client process is
  // already gone the user closed the app, so hand the tablet its own rotation
  // and screen behaviour back right away. Waiting for the retry loop cannot
  // work here because MIUI revives the process (and focuses it) within about
  // three seconds, which looks exactly like a user who wants the app running.
  if (was_connected && !serial.empty()) {
    const std::stop_token no_cancel;
    if (!client_.IsClientRunning(serial, no_cancel)) {
      bool should_restore = false;
      {
        std::scoped_lock lock(mutex_);
        if (!device_restored_) {
          device_restored_ = true;
          should_restore = true;
        }
      }
      if (should_restore) {
        SessionTrace("restore: client gone at channel loss, returning tablet settings");
        (void)client_.RestoreDevice(serial, no_cancel);
      }
    }
  }
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
        case DeviceSelectionStatus::NoDevices:
          error = "未检测到平板：请插好 USB 数据线，并在平板「设置 → 开发者选项」中打开 USB 调试";
          break;
        case DeviceSelectionStatus::Unauthorized:
          error = "平板未授权：请在平板上点「允许 USB 调试」，并勾选「一律允许使用这台计算机」";
          break;
        case DeviceSelectionStatus::Offline:
          error = "平板离线：请重新插拔 USB 数据线";
          break;
        case DeviceSelectionStatus::MultipleDevices:
          error = "检测到多台设备：请只保留一台平板连接";
          break;
        case DeviceSelectionStatus::MixedStates:
          error = "设备状态异常：请重新插拔 USB 数据线";
          break;
        case DeviceSelectionStatus::UnknownState:
          error = "设备状态未知：请重新插拔 USB 数据线";
          break;
        case DeviceSelectionStatus::CommandFailed:
          error = "adb 命令执行失败：请检查 USB 数据线与驱动";
          break;
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
  // Apply the device-side prerequisites on every connect so a ROM update or a
  // settings reset cannot silently reintroduce the portrait/sleep failure.
  (void)client_.PrepareDevice(serial, token);
  const ProcessResult reverse = client_.InstallReverseMappings(
      serial, options_.ports, token);
  if (!reverse.succeeded()) {
    ScheduleReconnect(reverse.stderr_text.empty() ? "ADB reverse setup failed"
                                                   : reverse.stderr_text);
    SessionTrace("reverse failed: " + reverse.stderr_text.substr(0, 160));
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
  SessionTrace("launch: succeeded=" + std::to_string(launch.succeeded() ? 1 : 0) +
               " err=" + launch.stderr_text.substr(0, 120));
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

bool AdbSession::CanStartNewAttempt() {
  // These probes must not be cancelled: stop_source_ is deliberately stopped
  // whenever a channel is lost, so reusing its token would abort every listing
  // and pidof call and make the host believe the device vanished.
  const std::stop_token cancel;
  const DeviceListing listing = client_.SelectSingleAuthorizedDevice(cancel);
  const bool device_present =
      listing.status == DeviceSelectionStatus::Authorized &&
      listing.selected.has_value();

  std::scoped_lock lock(mutex_);
  if (shutdown_requested_) return false;

  if (!device_present) {
    if (device_was_present_) {
      // The tablet was unplugged: arm auto-start for the next plug-in.
      device_was_present_ = false;
      ++device_generation_;
      had_session_this_generation_ = false;
      awaiting_replug_ = false;
      backoff_.reset();
      snapshot_.next_retry = clock_.Now();
    }
    snapshot_.last_error =
        "未检测到平板：请插好 USB 数据线，并在平板「设置 → 开发者选项」中打开 USB 调试";
    return false;
  }

  if (!device_was_present_) {
    device_was_present_ = true;
    ++device_generation_;
    had_session_this_generation_ = false;
    awaiting_replug_ = false;
    suppress_adopt_until_ = {};
    device_restored_ = false;
    backoff_.reset();
    snapshot_.next_retry = clock_.Now();
  }

  const std::string serial = listing.selected->serial;
  const bool client_running = client_.IsClientRunning(serial, cancel);
  // A process that is merely alive is not a user request: Android may restart
  // the app in the background after a force-stop. Only a foreground client
  // counts as "the user opened it".
  const bool client_foreground =
      client_running && client_.IsClientForeground(serial, cancel);

  // A session that has already been established is the boundary between
  // "start it for the user" and "the user is in control". Before that boundary
  // the host keeps retrying; afterwards it only steps in while the app is
  // actually in front.
  if (had_session_this_generation_) {
    // Never relaunch during the grace window: a just-closed process can still
    // look alive, and relaunching then would resurrect what the user closed.
    if (!awaiting_replug_ && channel_loss_at_ != std::chrono::steady_clock::time_point{} &&
        clock_.Now() - channel_loss_at_ < kClientGoneGrace) {
      snapshot_.next_retry = clock_.Now() + std::chrono::milliseconds(500);
      return false;
    }
    if (awaiting_replug_ && clock_.Now() < suppress_adopt_until_) {
      snapshot_.next_retry = clock_.Now() + kClientPollInterval;
      return false;
    }
    if (!client_foreground) {
      // Either the user closed the app or switched away from it, or Android
      // restarted the process in the background. In all three cases staying
      // stopped is what the user asked for; take over again only once the app
      // is brought back to the foreground (or the tablet is replugged).
      awaiting_replug_ = true;
      backoff_.reset();
      suppress_adopt_until_ = clock_.Now() + kUserStopCooldown;
      snapshot_.last_error =
          "平板端已停止：请在平板上重新打开 PadDrawBoard，或重新插拔 USB 数据线";
      // Plan 1: the session is over and the app is not in front, so the user
      // is no longer using the tablet as a display. Hand its own rotation and
      // screen behaviour back. Gating on "process gone" instead would rarely
      // fire, because MIUI revives the process within seconds.
      if (!device_restored_) {
        SessionTrace("restore: client closed, returning tablet settings to defaults");
        (void)client_.RestoreDevice(serial);
        device_restored_ = true;
      }
      snapshot_.next_retry = clock_.Now() + kClientPollInterval;
      return false;
    }
    // Foreground client without a session: adopt it with a fresh token.
    awaiting_replug_ = false;
    device_restored_ = false;
    backoff_.reset();
    snapshot_.next_retry = clock_.Now();
    return true;
    SessionTrace("decision: adopt foreground client with fresh token");
  }

  return true;
}

void AdbSession::ScheduleReconnect(std::string error) {
  SessionTrace("schedule reconnect: " + error.substr(0, 160));
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
