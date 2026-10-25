#include "pdb/app/desktop_server.h"
#include "pdb/app/client_handshake_watchdog.h"

#include "pdb/app/framed_stream.h"
#include "pdb/app/session_auth.h"
#include "pdb/adb/client.h"
#include "pdb/video/frame_pacer.h"
#include "pdb/adb/input_probe.h"
#include "pdb/adb/process.h"
#include "pdb/adb/session.h"
#include "pdb/video/monitor_enumerator.h"
#include "pdb/video/resolution_ladder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

namespace pdb::app {

namespace testing {

enum class VideoBackpressureAction : std::uint8_t {
  kNone,
  kNotifyIdr,
  kEndSession,
};

VideoBackpressureAction ObserveVideoBackpressure(
    bool* idr_notified, std::int64_t* first_blocked_ns,
    std::int64_t now_ns) noexcept {
  constexpr std::int64_t kTimeoutNs = 2'000'000'000;
  if (*first_blocked_ns == 0) *first_blocked_ns = now_ns;
  if (!*idr_notified) {
    *idr_notified = true;
    return VideoBackpressureAction::kNotifyIdr;
  }
  return now_ns >= *first_blocked_ns && now_ns - *first_blocked_ns >= kTimeoutNs
             ? VideoBackpressureAction::kEndSession
             : VideoBackpressureAction::kNone;
}

void ClearVideoBackpressure(bool* idr_notified,
                            std::int64_t* first_blocked_ns) noexcept {
  *idr_notified = false;
  *first_blocked_ns = 0;
}

}  // namespace testing

namespace {

using paddrawboard::protocol::ClientHello;
using paddrawboard::protocol::ClockSyncComplete;
using paddrawboard::protocol::ClockSyncRequest;
using paddrawboard::protocol::ClockSyncResponse;
using paddrawboard::protocol::Control;
using paddrawboard::protocol::Disconnect;
using paddrawboard::protocol::Frame;
using paddrawboard::protocol::FrameHeader;
using paddrawboard::protocol::InputBatch;
using paddrawboard::protocol::MessageType;
using paddrawboard::protocol::OrientationChanged;
using paddrawboard::protocol::RequestIdr;
using paddrawboard::protocol::ServerConfig;
using paddrawboard::protocol::Telemetry;
using paddrawboard::protocol::VideoFrame;

constexpr int kControlHandshakeTimeoutMs = 5'000;
constexpr int kChannelHandshakeTimeoutMs = 5'000;
constexpr int kSocketReceiveTimeoutMs = 1'000;

constexpr int kSocketSendTimeoutMs = 50;

std::uint64_t SteadyNowNs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t SteadyTimeNs(video::SteadyTime value) noexcept {
  if (value == video::SteadyTime{}) return 0;
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
      value.time_since_epoch()).count();
  return count <= 0 ? 0 : static_cast<std::uint64_t>(count);
}

std::string HResultText(HRESULT value) {
  std::ostringstream output;
  output << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(value);
  return output.str();
}

std::optional<input::Rotation> ToRotation(std::uint16_t degrees) {
  switch (degrees) {
    case 0: return input::Rotation::k0;
    case 90: return input::Rotation::k90;
    case 180: return input::Rotation::k180;
    case 270: return input::Rotation::k270;
    default: return std::nullopt;
  }
}

video::Size MonitorSize(const video::MonitorInfo& monitor) {
  return {static_cast<std::uint32_t>(monitor.desktop_rect.right - monitor.desktop_rect.left),
          static_cast<std::uint32_t>(monitor.desktop_rect.bottom - monitor.desktop_rect.top)};
}

input::PixelRect MonitorRect(const video::MonitorInfo& monitor) {
  return {monitor.desktop_rect.left, monitor.desktop_rect.top,
          monitor.desktop_rect.right - monitor.desktop_rect.left,
          monitor.desktop_rect.bottom - monitor.desktop_rect.top};
}

std::optional<std::pair<video::Size, std::uint32_t>> ChooseVideoSize(
    video::Size native, const ClientHello& hello) {
  for (const std::uint32_t longest_edge : video::kLongestEdgeLadder) {
    const video::Size candidate = video::SelectEncodeResolution(native, longest_edge);
    if (candidate.valid() && candidate.width <= hello.maxVideoWidth &&
        candidate.height <= hello.maxVideoHeight) {
      return std::make_pair(candidate, longest_edge);
    }
  }
  return std::nullopt;
}

Frame ControlFrame(Control control, std::uint32_t sequence) {
  Frame frame;
  frame.header = FrameHeader{0, sequence, MessageType::Control};
  frame.payload = std::move(control);
  return frame;
}

bool WriteDisconnect(SOCKET socket, std::uint32_t sequence, std::uint16_t reason,
                     std::string message) {
  if (socket == INVALID_SOCKET) return false;
  SocketByteStream stream(socket);
  const FrameIoResult result = FramedStream::WriteFrame(
      stream, ControlFrame(Disconnect{reason, std::move(message)}, sequence));
  return result.status == IoStatus::kOk;
}

void ShutdownAndClose(SOCKET socket) noexcept {
  if (socket != INVALID_SOCKET) {
    shutdown(socket, SD_BOTH);
    adb::CloseSocket(socket);
  }
}

bool IsH264Supported(const ClientHello& hello) noexcept {
  return (hello.videoCodecMask & paddrawboard::protocol::kCodecH264) != 0;
}

bool IsAllButtonCapabilityPresent(std::uint32_t capabilities) noexcept {
  constexpr std::uint32_t required = paddrawboard::protocol::kCapabilityButton1 |
                                     paddrawboard::protocol::kCapabilityButton2 |
                                     paddrawboard::protocol::kCapabilityButton3;
  return (capabilities & required) == required;
}

std::string CapabilityText(adb::CapabilityAvailability capability) {
  switch (capability) {
    case adb::CapabilityAvailability::Supported: return "supported";
    case adb::CapabilityAvailability::Unsupported: return "unsupported";
    case adb::CapabilityAvailability::Inaccessible: return "inaccessible";
    case adb::CapabilityAvailability::Unknown: return "unknown";
  }
  return "unknown";
}

std::string ProbeSummary(const adb::InputProbeResult& result) {
  const auto& capabilities = result.capabilities;
  return "pressure=" + CapabilityText(capabilities.pressure) +
         ", hover=" + CapabilityText(capabilities.hover) +
         ", tilt=" + CapabilityText(capabilities.tilt) +
         ", distance=" + CapabilityText(capabilities.distance) +
         ", touch=" + CapabilityText(capabilities.touch) +
         ", buttons=" + CapabilityText(capabilities.buttons[0]) + "/" +
         CapabilityText(capabilities.buttons[1]) + "/" +
         CapabilityText(capabilities.buttons[2]);
}

std::uint64_t NewSessionId() {
  static std::random_device device;
  static std::mt19937_64 random(device());
  std::uint64_t value = random();
  return value == 0 ? 1 : value;
}

bool ReadAuthPreface(SOCKET socket, adb::AdbSession& session,
                     AuthChannel channel, std::string* error) {
  std::array<std::uint8_t, kAuthPrefaceBytes> preface{};
  SocketByteStream stream(socket);
  std::size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kControlHandshakeTimeoutMs);
  while (offset < preface.size() &&
         std::chrono::steady_clock::now() < deadline) {
    const IoResult result = stream.Read(std::span<std::uint8_t>(preface).subspan(offset));
    if (result.transferred > preface.size() - offset) {
      if (error != nullptr) *error = "invalid authentication preface length";
      return false;
    }
    offset += result.transferred;
    if (result.status == IoStatus::kOk) {
      if (result.transferred == 0) break;
      continue;
    }
    if (result.status == IoStatus::kWouldBlock) continue;
    if (error != nullptr) *error = result.error.empty()
        ? "authentication preface connection closed" : result.error;
    return false;
  }
  if (offset != preface.size()) {
    if (error != nullptr) *error = "authentication preface timed out";
    return false;
  }
  if (!std::equal(kAuthMagic.begin(), kAuthMagic.end(), preface.begin()) ||
      preface[kAuthMagic.size()] != static_cast<std::uint8_t>(channel)) {
    if (error != nullptr) *error = "unauthenticated local channel";
    return false;
  }
  const auto token = std::span<const std::uint8_t>(preface).subspan(
      kAuthMagic.size() + 1, adb::kSessionTokenBytes);
  if (!session.ValidateSessionToken(token)) {
    if (error != nullptr) *error = "stale or invalid session token";
    return false;
  }
  return true;
}

bool ReceiveChannel(adb::LoopbackListener& listener, std::atomic<bool>& running,
                    SOCKET* result, std::string* error,
                    adb::AdbSession& session, AuthChannel channel) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kChannelHandshakeTimeoutMs);
  while (running.load() && std::chrono::steady_clock::now() < deadline) {
    const auto status = listener.Accept(result, 100, error);
    if (status == adb::LoopbackListener::AcceptStatus::Accepted) {
      if (!SocketByteStream::SetTimeouts(*result, kSocketReceiveTimeoutMs,
                                         kSocketSendTimeoutMs)) {
        if (error != nullptr) *error = "cannot configure channel socket timeouts";
        return false;
      }
      if (!ReadAuthPreface(*result, session, channel, error)) {
        ShutdownAndClose(*result);
        *result = INVALID_SOCKET;
        return false;
      }
      return true;
    }
    if (status == adb::LoopbackListener::AcceptStatus::Error) return false;
  }
  if (error != nullptr && error->empty()) *error = "Android channel connection timed out";
  return false;
}

}  // namespace

std::filesystem::path TelemetryWriter::DefaultPath() {
  const DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
  if (needed > 1) {
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    if (GetEnvironmentVariableW(L"APPDATA", value.data(), needed) == needed - 1) {
      value.resize(needed - 1);
      return std::filesystem::path(value) / L"PadDrawBoard" / L"telemetry.jsonl";
    }
  }
  return std::filesystem::current_path() / L"PadDrawBoard" / L"telemetry.jsonl";
}

bool TelemetryWriter::OpenForAppend() {
  if (output_.is_open()) {
    if (output_) return true;
    CloseStream();
    return false;
  }

  std::error_code error;
  if (!path_.parent_path().empty()) {
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) return false;
  }
  std::uintmax_t existing = 0;
  if (std::filesystem::exists(path_, error)) {
    if (error) return false;
    existing = std::filesystem::file_size(path_, error);
    if (error) return false;
  } else if (error) {
    return false;
  }
  output_.open(path_, std::ios::binary | std::ios::app);
  if (!output_) {
    output_.clear();
    current_bytes_ = 0;
    return false;
  }
  current_bytes_ = existing;
  return true;
}

void TelemetryWriter::CloseStream() const noexcept {
  if (output_.is_open()) output_.close();
  output_.clear();
  current_bytes_ = 0;
}

bool TelemetryWriter::FlushStream() const noexcept {
  if (!output_.is_open()) return true;
  output_.flush();
  if (output_) return true;
  CloseStream();
  return false;
}

bool TelemetryWriter::RotateIfNeeded(std::uintmax_t incoming_bytes) {
  if (current_bytes_ == 0 ||
      incoming_bytes <= kMaximumBytes -
          (current_bytes_ < kMaximumBytes ? current_bytes_ : kMaximumBytes)) {
    return true;
  }

  if (!FlushStream()) return false;
  CloseStream();

  std::error_code error;
  for (unsigned index = kRetainedRotations; index > 0; --index) {
    const std::filesystem::path source = index == 1
        ? path_ : std::filesystem::path(path_.wstring() + L"." + std::to_wstring(index - 1));
    const std::filesystem::path destination =
        std::filesystem::path(path_.wstring() + L"." + std::to_wstring(index));
    std::filesystem::remove(destination, error);
    if (error) return OpenForAppend();
    if (!std::filesystem::exists(source, error)) {
      if (error) return OpenForAppend();
      continue;
    }
    std::filesystem::rename(source, destination, error);
    if (error) return OpenForAppend();
  }
  return OpenForAppend();
}

void TelemetryWriter::Write(std::string_view line) {
  if (line.empty()) return;
  std::scoped_lock lock(mutex_);
  const auto incoming_bytes = static_cast<std::uintmax_t>(line.size() + 1);
  if (!OpenForAppend() || !RotateIfNeeded(incoming_bytes)) return;
  output_.write(line.data(), static_cast<std::streamsize>(line.size()));
  output_.put('\n');
  output_.flush();
  if (!output_) {
    CloseStream();
    return;
  }
  current_bytes_ += incoming_bytes;
}

std::uint64_t TelemetryWriter::BeginRun() {
  std::scoped_lock lock(mutex_);
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) return 0;
  const std::filesystem::path marker(path_.wstring() + L".running");
  const std::filesystem::path count_path(path_.wstring() + L".crashes");
  std::uint64_t count = 0;
  {
    std::ifstream input(count_path, std::ios::binary);
    if (input) input >> count;
  }
  if (std::filesystem::exists(marker, error) && count != UINT64_MAX) ++count;
  error.clear();
  std::ofstream count_output(count_path, std::ios::binary | std::ios::trunc);
  if (count_output) count_output << count;
  std::ofstream marker_output(marker, std::ios::binary | std::ios::trunc);
  if (marker_output) {
    marker_output << "running\n";
    run_marker_active_ = true;
  }
  return count;
}

void TelemetryWriter::EndRun() noexcept {
  std::scoped_lock lock(mutex_);
  (void)FlushStream();
  if (!run_marker_active_) return;
  std::error_code error;
  std::filesystem::remove(std::filesystem::path(path_.wstring() + L".running"), error);
  run_marker_active_ = false;
}

bool TelemetryWriter::CopyTo(const std::filesystem::path& destination,
                             std::string* error) const {
  std::scoped_lock lock(mutex_);
  if (destination == path_) return true;
  if (!FlushStream()) {
    if (error != nullptr) *error = "cannot flush telemetry file";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::exists(path_, filesystem_error)) {
    if (error != nullptr) *error = "telemetry file does not exist";
    return false;
  }
  if (!destination.parent_path().empty()) {
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
      if (error != nullptr) *error = "cannot create telemetry export directory";
      return false;
    }
  }
  std::ifstream source(path_, std::ios::binary);
  std::ofstream target(destination, std::ios::binary | std::ios::trunc);
  if (!source || !target) {
    if (error != nullptr) *error = "cannot open telemetry export";
    return false;
  }
  target << source.rdbuf();
  if (!target) {
    if (error != nullptr) *error = "cannot write telemetry export";
    return false;
  }
  return true;
}

DesktopServer::DesktopServer(ServerOptions options) : options_(std::move(options)) {}

DesktopServer::~DesktopServer() { Stop(); }

bool DesktopServer::Start() {
  if (running_.exchange(true)) return true;
  ClearPendingClockSync();
  const AppConfig config = CurrentConfig();
  std::string error;
  if (!ConfigStore::Validate(config, &error)) {
    running_.store(false);
    UpdateDiagnostic("invalid configuration: " + error);
    return false;
  }
  input_.SetPalmRejectionEnabled(config.palm_guard_enabled);
  input_.SetProfiles(config.profiles);
  if (!control_listener_.Open(adb::kControlPort, 1, &error) ||
      !video_listener_.Open(adb::kVideoPort, 1, &error) ||
      !input_listener_.Open(adb::kInputPort, 1, &error)) {
    control_listener_.Close();
    video_listener_.Close();
    input_listener_.Close();
    running_.store(false);
    UpdateDiagnostic("cannot bind 127.0.0.1: " + error);
    return false;
  }
  process_runner_ = std::make_unique<adb::Win32ProcessRunner>();
  clock_ = std::make_unique<adb::SteadyClock>();
  const std::uint64_t crash_count = telemetry_.BeginRun();
  {
    std::scoped_lock lock(mutex_);
    status_ = {};
    status_.phase = ServerPhase::kAwaitingAdb;
    status_.input_injection_available = input_.IsInjectionAvailable();
    status_.crash_count = crash_count;
    status_.release_ready = false;
    status_.diagnostic = "loopback listeners ready; waiting for authorized ADB device";
  }
  current_client_capabilities_.store(0, std::memory_order_release);
  accept_thread_ = std::jthread([this](std::stop_token stop_token) { AcceptLoop(stop_token); });
  adb_thread_ = std::jthread([this](std::stop_token stop_token) { AdbLoop(stop_token); });
  return true;
}

void DesktopServer::RestoreTabletForExit() noexcept {
  adb::AdbSession* session = nullptr;
  {
    std::scoped_lock lock(mutex_);
    session = adb_session_.get();
  }
  if (session != nullptr) session->RestoreDeviceNow();
}

void DesktopServer::Stop() noexcept {
  const bool was_running = running_.exchange(false);
  if (!was_running && !accept_thread_.joinable() && !adb_thread_.joinable()) return;
  // Tell the ADB session to stop attempting launches *before* the threads are
  // joined. Otherwise the retry loop keeps running during teardown and can
  // relaunch the tablet client right as the user asks to quit, which both slows
  // the exit down and fights the "close it and it stays closed" behaviour.
  {
    adb::AdbSession* session = nullptr;
    {
      std::scoped_lock lock(mutex_);
      session = adb_session_.get();
    }
    if (session != nullptr) session->BeginShutdown();
  }
  EndSession("desktop server shutting down");
  control_listener_.Close();
  video_listener_.Close();
  input_listener_.Close();
  accept_thread_.request_stop();
  adb_thread_.request_stop();
  control_thread_.request_stop();
  input_thread_.request_stop();
  video_thread_.request_stop();
  if (accept_thread_.joinable()) accept_thread_.join();
  if (control_thread_.joinable()) control_thread_.join();
  if (input_thread_.joinable()) input_thread_.join();
  if (video_thread_.joinable()) video_thread_.join();
  if (adb_thread_.joinable()) adb_thread_.join();
  if (adb_session_) adb_session_->Stop();
  adb_session_.reset();
  adb_client_.reset();
  clock_.reset();
  process_runner_.reset();
  telemetry_.EndRun();
  {
    std::scoped_lock lock(mutex_);
    status_.phase = ServerPhase::kStopped;
    status_.control_connected = false;
    status_.video_connected = false;
    status_.input_connected = false;
    status_.input_probe_completed = false;
    status_.pen_buttons_confirmed = false;
    status_.release_ready = false;
    status_.capture_started = false;
    status_.diagnostic = "desktop server stopped";
  }
  current_client_capabilities_.store(0, std::memory_order_release);
}

ServerStatus DesktopServer::Status() const {
  std::scoped_lock lock(mutex_);
  return status_;
}

AppConfig DesktopServer::CurrentConfig() const {
  std::scoped_lock lock(config_mutex_);
  return options_.config;
}

bool DesktopServer::Reconfigure(AppConfig config, std::string* error) {
  if (!ConfigStore::Validate(config, error)) return false;
  if (!options_.config_store.Save(config, error)) return false;
  const bool was_running = running_.load();
  if (was_running) Stop();
  {
    std::scoped_lock lock(config_mutex_);
    options_.config = std::move(config);
  }
  if (was_running && !Start()) {
    if (error != nullptr) *error = Status().diagnostic;
    return false;
  }
  return true;
}

void DesktopServer::AcceptLoop(std::stop_token stop_token) {
  while (!stop_token.stop_requested() && running_.load()) {
    if (session_active_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      continue;
    }
    SOCKET control = INVALID_SOCKET;
    std::string error;
    const auto result = control_listener_.Accept(&control, 100, &error);
    if (result == adb::LoopbackListener::AcceptStatus::Timeout) continue;
    if (result == adb::LoopbackListener::AcceptStatus::Error) {
      if (running_.load()) UpdateDiagnostic("control listener error: " + error);
      continue;
    }
    if (!EstablishSession(control)) ShutdownAndClose(control);
  }
}

void DesktopServer::AdbLoop(std::stop_token stop_token) {
  ClientHandshakeWatchdog handshake_watchdog;
  while (!stop_token.stop_requested() && running_.load()) {
    bool probe_attempted = false;
    adb::AdbLocatorOptions options;
    options.executable_directory = options_.executable_directory;
    options.fallback_path = options_.executable_directory / L"adb.exe";
    adb::AdbLocator locator(*process_runner_);
    const auto executable = locator.Locate(options);
    if (!executable) {
      {
        std::scoped_lock lock(mutex_);
        if (!session_active_.load()) status_.phase = ServerPhase::kAwaitingAdb;
        status_.diagnostic = "ADB 36.0.2 not found beside PadDrawBoard";
      }
      for (int attempt = 0; attempt < 20 && !stop_token.stop_requested() && running_.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }
    auto client = std::make_unique<adb::AdbClient>(executable->path, *process_runner_);
    adb::AdbClient* client_pointer = client.get();
    auto session = std::make_unique<adb::AdbSession>(*client, *clock_);
    {
      std::scoped_lock lock(mutex_);
      adb_client_ = std::move(client);
      adb_session_ = std::move(session);
    }
    (void)adb_session_->Start();
    while (!stop_token.stop_requested() && running_.load()) {
      adb_session_->Tick();
      const bool connected = adb_session_->connected();
      if (handshake_watchdog.Observe(
              connected, session_active_.load(std::memory_order_acquire),
              std::chrono::steady_clock::now())) {
        adb_session_->NotifyChannelLoss(
            "Android client authentication handshake timed out; relaunching with a fresh token");
        probe_attempted = false;
        continue;
      }
      if (connected && !probe_attempted) {
        const adb::DeviceListing selected = client_pointer->SelectSingleAuthorizedDevice(stop_token);
        if (selected.status == adb::DeviceSelectionStatus::Authorized && selected.selected.has_value()) {
          const adb::InputProbe probe(*client_pointer);
          const adb::InputProbeResult result = probe.Probe(selected.selected->serial, {}, stop_token);
          const bool buttons_confirmed = result.command_succeeded &&
              result.capabilities.all_buttons_observable();
          {
            std::scoped_lock lock(mutex_);
            status_.input_probe_completed = true;
            status_.pen_buttons_confirmed = buttons_confirmed;
            status_.input_probe_diagnostic = ProbeSummary(result);
            UpdateReleaseReadinessLocked();
          }
          telemetry_.Write(
              "{\"kind\":\"input_probe\",\"command_succeeded\":" +
              std::string(result.command_succeeded ? "true" : "false") +
              ",\"permission_denied\":" +
              std::string(result.permission_denied ? "true" : "false") +
              ",\"all_buttons_confirmed\":" +
              std::string(buttons_confirmed ? "true" : "false") + "}");
          probe_attempted = true;
        }
      } else if (!connected && probe_attempted) {
        probe_attempted = false;
        std::scoped_lock lock(mutex_);
        status_.input_probe_completed = false;
        status_.pen_buttons_confirmed = false;
        UpdateReleaseReadinessLocked();
      }
      {
        std::scoped_lock lock(mutex_);
        if (!session_active_.load()) {
          status_.phase = connected ? ServerPhase::kAwaitingClient : ServerPhase::kAwaitingAdb;
          if (!connected) status_.diagnostic = adb_session_->Snapshot().last_error;
          else status_.diagnostic = "ADB reverse active; waiting for Android ClientHello";
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    adb_session_->Stop();
    return;
  }
}

bool DesktopServer::EstablishSession(SOCKET control_socket) {
  const AppConfig config = CurrentConfig();
  adb::AdbSession* adb_session = nullptr;
  {
    std::scoped_lock lock(mutex_);
    adb_session = adb_session_.get();
  }
  if (adb_session == nullptr) {
    UpdateDiagnostic("rejected control peer without an ADB session");
    return false;
  }
  if (!SocketByteStream::SetTimeouts(control_socket, kControlHandshakeTimeoutMs,
                                     kSocketSendTimeoutMs)) {
    UpdateDiagnostic("cannot configure control socket timeouts");
    return false;
  }
  SocketByteStream control_stream(control_socket);
  std::string auth_error;
  if (!ReadAuthPreface(control_socket, *adb_session, AuthChannel::Control,
                       &auth_error)) {
    UpdateDiagnostic(auth_error.empty() ? "unauthenticated control channel" :
                                         auth_error);
    return false;
  }
  // The client is authenticated, so the session is real from the host's point
  // of view. This is what arms the "user closed the app" detection.
  adb_session->NotifySessionEstablished();
  Frame handshake;
  const FrameIoResult read = FramedStream::ReadFrame(control_stream, &handshake);
  if (read.status != IoStatus::kOk || handshake.header.type != MessageType::ClientHello) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 1,
                    "expected a valid ClientHello on the control channel");
    UpdateDiagnostic(read.error.empty() ? "invalid ClientHello" : read.error);
    return false;
  }
  const ClientHello* hello = std::get_if<ClientHello>(&handshake.payload);
  if (hello == nullptr || !IsH264Supported(*hello)) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 2,
                    "PadDrawBoard v1 requires H.264 decode support");
    UpdateDiagnostic("Android client does not advertise H.264");
    return false;
  }
  std::vector<video::MonitorInfo> monitors;
  const HRESULT enumerated = video::MonitorEnumerator::Enumerate(&monitors);
  if (FAILED(enumerated) || monitors.empty()) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 4,
                    "no capturable Windows monitor is available");
    UpdateDiagnostic(FAILED(enumerated) ? HResultText(enumerated) : "no capturable monitor");
    return false;
  }
  video::MonitorInfo monitor = monitors.front();
  if (!config.selected_monitor_id.empty()) {
    video::MonitorId requested;
    if (video::MonitorId::TryParse(config.selected_monitor_id, &requested)) {
      const auto found = std::find_if(monitors.begin(), monitors.end(), [&](const auto& item) {
        return item.id == requested;
      });
      if (found != monitors.end()) monitor = *found;
    }
  }
  const auto chosen_video = ChooseVideoSize(MonitorSize(monitor), *hello);
  if (!chosen_video) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 5,
                    "selected monitor cannot fit the client H.264 resolution limits");
    UpdateDiagnostic("client video dimensions are below the 1920px v1 ladder");
    return false;
  }
  const auto rotation = ToRotation(hello->rotationDegrees);
  if (!rotation) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 6, "invalid Android orientation");
    return false;
  }
  const std::uint64_t session_id = NewSessionId();
  ServerConfig configuration;
  configuration.sessionId = session_id;
  configuration.displayId = monitor.id.target_id;
  configuration.videoWidth = static_cast<std::uint16_t>(chosen_video->first.width);
  configuration.videoHeight = static_cast<std::uint16_t>(chosen_video->first.height);
  configuration.videoFps = 60;
  configuration.videoBitrateMbps = static_cast<std::uint16_t>(config.bitrate_mbps);
  configuration.contentLeft = 0;
  configuration.contentTop = 0;
  configuration.contentWidth = chosen_video->first.width;
  configuration.contentHeight = chosen_video->first.height;
  configuration.configFlags = config.palm_guard_enabled
                                  ? paddrawboard::protocol::kConfigFlagSuppressTouchWhilePenInRange : 0;
  Frame response;
  response.header = FrameHeader{0, control_sequence_.fetch_add(1), MessageType::ServerConfig};
  response.payload = configuration;
  if (FramedStream::WriteFrame(control_stream, response).status != IoStatus::kOk) {
    UpdateDiagnostic("failed to send ServerConfig");
    return false;
  }
  if (!SocketByteStream::SetTimeouts(control_socket, kSocketReceiveTimeoutMs,
                                     kSocketSendTimeoutMs)) {
    UpdateDiagnostic("cannot configure control socket timeouts");
    return false;
  }

  SOCKET video_socket = INVALID_SOCKET;
  SOCKET input_socket = INVALID_SOCKET;
  std::string channel_error;
  if (!ReceiveChannel(video_listener_, running_, &video_socket, &channel_error,
                      *adb_session, AuthChannel::Video) ||
      !ReceiveChannel(input_listener_, running_, &input_socket, &channel_error,
                      *adb_session, AuthChannel::Input)) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 7, channel_error);
    ShutdownAndClose(video_socket);
    ShutdownAndClose(input_socket);
    UpdateDiagnostic(channel_error);
    return false;
  }

  const HRESULT video_start = [&] {
    std::scoped_lock lock(video_mutex_);
    return video_.Start(monitor, chosen_video->second, this);
  }();
  if (FAILED(video_start)) {
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 8,
                    "Windows capture or hardware H.264 pipeline is unavailable");
    ShutdownAndClose(video_socket);
    ShutdownAndClose(input_socket);
    UpdateDiagnostic("video start failed: " + HResultText(video_start));
    return false;
  }

  ++orientation_epoch_;
  input::MapperConfig mapper;
  mapper.monitor = MonitorRect(monitor);
  mapper.contentAspectRatio = static_cast<float>(chosen_video->first.width) /
                              static_cast<float>(chosen_video->first.height);
  // MotionEvent coordinates are already expressed in the currently rotated
  // Android View. The orientation value only starts a new input epoch; applying
  // it here would rotate the normalized View coordinates a second time.
  mapper.rotation = input::Rotation::k0;
  mapper.orientationEpoch = orientation_epoch_;
  if (!input_.Configure(mapper)) {
    std::scoped_lock lock(video_mutex_);
    video_.Stop();
    WriteDisconnect(control_socket, control_sequence_.fetch_add(1), 9,
                    "cannot configure monitor-to-tablet coordinate mapping");
    ShutdownAndClose(video_socket);
    ShutdownAndClose(input_socket);
    UpdateDiagnostic("input coordinate mapper rejected selected monitor");
    return false;
  }
  input_.SetPalmRejectionEnabled(config.palm_guard_enabled);
  input_.SetProfiles(config.profiles);
  {
    std::scoped_lock lock(input_adapter_mutex_);
    input_adapter_.Configure(*hello, orientation_epoch_);
  }
  AppConfig persisted_config = config;
  persisted_config.selected_monitor_id = monitor.id.ToString();
  std::string persist_error;
  if (!options_.config_store.Save(persisted_config, &persist_error)) {
    UpdateDiagnostic("session is active but config was not persisted: " + persist_error);
  } else {
    std::scoped_lock config_lock(config_mutex_);
    options_.config = persisted_config;
  }

  const std::uint64_t generation = generation_.fetch_add(1) + 1;
  {
    std::scoped_lock lock(socket_mutex_);
    control_socket_ = control_socket;
    video_socket_ = video_socket;
    input_socket_ = input_socket;
  }
  selected_monitor_ = monitor;
  configured_video_size_ = chosen_video->first;
  video_max_longest_edge_ = chosen_video->second;
  hello_ = *hello;
  current_client_capabilities_.store(hello->capabilities, std::memory_order_release);
  session_active_.store(true);
  {
    std::scoped_lock lock(mutex_);
    status_.phase = ServerPhase::kDegraded;
    status_.control_connected = true;
    status_.video_connected = true;
    status_.input_connected = true;
    status_.input_injection_available = input_.IsInjectionAvailable();
    status_.capture_started = true;
    status_.session_id = session_id;
    status_.video_frames_sent = 0;
    status_.stream_resets = 0;
    UpdateReleaseReadinessLocked();
    status_.diagnostic = IsAllButtonCapabilityPresent(hello->capabilities)
        ? "channels connected; validating hardware encoder" :
          "channels connected; validating hardware encoder; one or more pen buttons are unavailable";
  }
  if (control_thread_.joinable()) control_thread_.join();
  if (input_thread_.joinable()) input_thread_.join();
  if (video_thread_.joinable()) video_thread_.join();
  control_thread_ = std::jthread([this, generation](std::stop_token token) { ControlLoop(token, generation); });
  input_thread_ = std::jthread([this, generation](std::stop_token token) { InputLoop(token, generation); });
  video_thread_ = std::jthread([this, generation](std::stop_token token) { VideoLoop(token, generation); });
  return true;
}

void DesktopServer::ControlLoop(std::stop_token stop_token, std::uint64_t generation) {
  SOCKET socket = INVALID_SOCKET;
  {
    std::scoped_lock lock(socket_mutex_);
    socket = control_socket_;
  }
  SocketByteStream stream(socket);
  while (!stop_token.stop_requested() && IsGenerationActive(generation)) {
    Frame frame;
    const FrameIoResult read = FramedStream::ReadFrame(stream, &frame);
    if (read.status == IoStatus::kWouldBlock) continue;
    if (read.status != IoStatus::kOk || frame.header.type != MessageType::Control) {
      EndSession(read.error.empty() ? "control channel disconnected" : read.error);
      return;
    }
    const Control* control = std::get_if<Control>(&frame.payload);
    if (control == nullptr) {
      EndSession("invalid control payload");
      return;
    }
    bool accepted = true;
    std::visit([&](const auto& message) {
      using T = std::decay_t<decltype(message)>;
      if constexpr (std::is_same_v<T, ClockSyncRequest>) {
        const std::uint64_t received = SteadyNowNs();
        const std::uint64_t sent = SteadyNowNs();
        std::scoped_lock write_lock(control_write_mutex_);
        const FrameIoResult write = FramedStream::WriteFrame(
            stream, ControlFrame(ClockSyncResponse{message.clientSendTimestampNs, received, sent},
                                 control_sequence_.fetch_add(1)));
        accepted = write.status == IoStatus::kOk;
        if (accepted) {
          RecordPendingClockSync(message.clientSendTimestampNs, received, sent,
                                 generation, SteadyNowNs());
        }
      } else if constexpr (std::is_same_v<T, RequestIdr>) {
        std::scoped_lock video_lock(video_mutex_);
        (void)video_.NotifyTransportWouldBlock();
      } else if constexpr (std::is_same_v<T, OrientationChanged>) {
        const auto rotation = ToRotation(message.rotationDegrees);
        if (!rotation) {
          accepted = false;
          return;
        }
        ++orientation_epoch_;
        input::MapperConfig mapper;
        mapper.monitor = MonitorRect(selected_monitor_);
        mapper.contentAspectRatio = configured_video_size_.height == 0 ? 0.0F :
            static_cast<float>(configured_video_size_.width) /
                static_cast<float>(configured_video_size_.height);
        // The Android View has already transformed MotionEvent coordinates for
        // the new display orientation. Keep host mapping in View coordinates.
        mapper.rotation = input::Rotation::k0;
        mapper.orientationEpoch = orientation_epoch_;
        input_.OnOrientationChange(mapper);
        std::scoped_lock adapter_lock(input_adapter_mutex_);
        input_adapter_.SetOrientationEpoch(orientation_epoch_);
      } else if constexpr (std::is_same_v<T, paddrawboard::protocol::CapabilityChanged>) {
        current_client_capabilities_.store(message.capabilities, std::memory_order_release);
        std::scoped_lock adapter_lock(input_adapter_mutex_);
        input_adapter_.SetCapabilities(message.capabilities);
        {
          std::scoped_lock status_lock(mutex_);
          UpdateReleaseReadinessLocked();
        }
        if (!IsAllButtonCapabilityPresent(message.capabilities)) {
          UpdateDiagnostic("Android client reports unavailable pen buttons");
        }
      } else if constexpr (std::is_same_v<T, Telemetry>) {
        std::scoped_lock lock(mutex_);
        status_.client_rtt_us = message.rttUs;
        status_.client_video_latency_us = message.videoLatencyUs;
        UpdateReleaseReadinessLocked();
        telemetry_.Write(
            "{\"kind\":\"client_telemetry\",\"timestamp_ns\":" +
            std::to_string(SteadyNowNs()) + ",\"rtt_us\":" +
            std::to_string(message.rttUs) + ",\"video_latency_us\":" +
            std::to_string(message.videoLatencyUs) +
            ",\"dropped_video_frames\":" +
            std::to_string(message.droppedVideoFrames) + "}");
        telemetry_.Write(TelemetryJson::Soak(
            SteadyNowNs(), 0, status_.active_pointers, status_.crash_count,
            status_.stream_resets,
            static_cast<double>(message.videoLatencyUs) / 1000.0));
      } else if constexpr (std::is_same_v<T, Disconnect>) {
        accepted = false;
      } else if constexpr (std::is_same_v<T, ClockSyncResponse>) {
        // Responses belong to the Android clock estimator. The host only emits them.
      } else if constexpr (std::is_same_v<T, ClockSyncComplete>) {
        const std::string error = ConsumeClockSyncComplete(
            message, generation, SteadyNowNs());
        if (error.empty()) {
          telemetry_.Write(TelemetryJson::ClockSync(
              message.deviceSendNs, message.hostReceiveNs,
              message.hostSendNs, message.deviceReceiveNs));
        } else {
          UpdateDiagnostic("ignored ClockSyncComplete: " + error);
        }
      }
    }, *control);
    if (!accepted) {
      EndSession("control channel requested or failed session shutdown");
      return;
    }
  }
}

void DesktopServer::InputLoop(std::stop_token stop_token, std::uint64_t generation) {
  SOCKET socket = INVALID_SOCKET;
  {
    std::scoped_lock lock(socket_mutex_);
    socket = input_socket_;
  }
  SocketByteStream stream(socket);
  while (!stop_token.stop_requested() && IsGenerationActive(generation)) {
    Frame frame;
    const FrameIoResult read = FramedStream::ReadFrame(stream, &frame);
    if (read.status == IoStatus::kWouldBlock) continue;
    if (read.status != IoStatus::kOk || frame.header.type != MessageType::InputBatch) {
      EndSession(read.error.empty() ? "input channel disconnected" : read.error);
      return;
    }
    const InputBatch* batch = std::get_if<InputBatch>(&frame.payload);
    input::InputFrame adapted;
    std::string adapter_error;
    if (batch == nullptr || [&] {
          std::scoped_lock adapter_lock(input_adapter_mutex_);
          return !input_adapter_.Adapt(*batch, std::chrono::steady_clock::now(), &adapted, &adapter_error);
        }()) {
      EndSession(adapter_error.empty() ? "invalid InputBatch payload" : adapter_error);
      return;
    }
    const std::uint64_t received_ns = SteadyNowNs();
    UpdateActivePointers(*batch);
    const std::uint64_t sent_ns = batch->batchTimestampNs;
    const std::uint64_t latency_us = received_ns >= sent_ns
        ? (received_ns - sent_ns) / 1'000u : 0u;
    telemetry_.Write(TelemetryJson::InputTransport(
        sent_ns, received_ns, latency_us, Status().active_pointers));
    if (!input_.Process(adapted)) {
      std::scoped_lock lock(mutex_);
      status_.phase = ServerPhase::kDegraded;
      status_.input_injection_available = input_.IsInjectionAvailable();
      UpdateReleaseReadinessLocked();
      status_.diagnostic = status_.input_injection_available
          ? "input injection rejected a frame" : "Windows synthetic pointer injection is unavailable";
    }
  }
}

void DesktopServer::VideoLoop(std::stop_token stop_token, std::uint64_t generation) {
  ResumableFrameWriter pending_frame;
  std::uint32_t pending_sequence{};
  std::uint64_t pending_capture_timestamp_ns{};
  std::uint64_t pending_presentation_timestamp_ns{};
  bool backpressure_idr_notified = false;
  std::int64_t backpressure_started_ns = 0;
  std::condition_variable_any pending_encode_pause;
  std::mutex pending_encode_pause_mutex;
  video::EncodedAccessUnit encoded;
  std::chrono::steady_clock::time_point next_frame_at{};
  video::FramePacer frame_pacer;
  while (!stop_token.stop_requested() && IsGenerationActive(generation)) {
    SOCKET socket = INVALID_SOCKET;
    {
      std::scoped_lock lock(socket_mutex_);
      socket = video_socket_;
    }
    if (socket == INVALID_SOCKET) {
      EndSession("video socket closed");
      return;
    }
    SocketByteStream stream(socket);
    if (pending_frame.pending()) {
      const FrameIoResult write = pending_frame.Continue(stream);
      if (write.status == IoStatus::kOk) {
        telemetry_.Write(TelemetryJson::VideoSample(
            pending_capture_timestamp_ns, pending_presentation_timestamp_ns,
            pending_sequence, 0, 0, Status().stream_resets));
        {
          std::scoped_lock lock(mutex_);
          if (status_.video_frames_sent == 0) {
            status_.phase = status_.input_injection_available ? ServerPhase::kActive : ServerPhase::kDegraded;
            status_.diagnostic = status_.release_ready
                ? "session active"
                : "session active; release readiness blocked until all three pen buttons are confirmed";
          }
          ++status_.video_frames_sent;
        }
        testing::ClearVideoBackpressure(&backpressure_idr_notified,
                                         &backpressure_started_ns);
        continue;
      }
      if (write.status == IoStatus::kWouldBlock) {
        // The serialized frame and its offset remain owned by pending_frame.
        // Do not reset the encoder or start a second protocol frame here.
        const auto action = testing::ObserveVideoBackpressure(
            &backpressure_idr_notified, &backpressure_started_ns,
            static_cast<std::int64_t>(SteadyNowNs()));
        if (action == testing::VideoBackpressureAction::kNotifyIdr) {
          std::scoped_lock lock(video_mutex_);
          (void)video_.NotifyTransportWouldBlock();
        } else if (action == testing::VideoBackpressureAction::kEndSession) {
          EndSession("video transport blocked for 2 seconds");
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      // Even a fatal error after a partial write is a stream-level failure:
      // the receiver must discard the channel and resynchronize on a fresh
      // session/IDR. EndSession closes control/input too, keeping the three
      // channels from observing different generations.
      EndSession(write.error.empty() ? "video transport disconnected" : write.error);
      return;
    }

    // The encoder's media type does not throttle Desktop Duplication. In
    // particular fast encoders can otherwise run at the monitor's refresh
    // rate, wasting resources and flooding the 60-fps Android decoder.
    if (std::chrono::steady_clock::now() < next_frame_at) {
      frame_pacer.WaitUntil(next_frame_at);
      if (stop_token.stop_requested() || !IsGenerationActive(generation)) return;
    }
    HRESULT capture = S_FALSE;
    HRESULT encode = S_FALSE;
    bool capture_skipped_for_pending = false;
    std::string video_failure_stage;
    {
      std::scoped_lock lock(video_mutex_);
      // Poll the asynchronous encoder before touching Desktop Duplication.
      // A pending MFT input must never overlap AcquireNextFrame: on hybrid
      // GPU systems the two calls can deadlock in different driver queues.
      encode = video_.EncodeLatest(&encoded);
      const auto action = video::DecideVideoLoopAfterInitialEncode(encode);
      if (action == video::VideoLoopAfterEncodeAction::kCapture) {
        capture_skipped_for_pending = video_.has_pending_encode();
        capture = video_.CaptureOnce(16);
        if (video::ShouldEncodeAfterCapture(capture)) {
          encode = video_.EncodeLatest(&encoded);
        }
      if (FAILED(capture) || FAILED(encode)) {
        video_failure_stage = std::string(video_.last_failure_stage());
      }
      }
    }
    if (FAILED(capture) || FAILED(encode)) {
      EndSession("video pipeline unavailable at " + video_failure_stage + ": " +
                 HResultText(FAILED(capture) ? capture : encode));
      return;
    }
    if (video::ShouldYieldAfterPendingEncodeNoProgress(
            encode, capture, capture_skipped_for_pending)) {
      // The pending async input has not progressed, so CaptureOnce correctly
      // skipped AcquireNextFrame. Yield briefly without delaying the normal
      // 16ms Desktop Duplication timeout path. The stop token can interrupt
      // this wait during session shutdown.
      std::unique_lock pause_lock(pending_encode_pause_mutex);
      pending_encode_pause.wait_for(
          pause_lock, stop_token, std::chrono::milliseconds(1), [] { return false; });
      continue;
    }
    if (encode != S_OK || encoded.bytes.empty()) continue;
    next_frame_at = encoded.acquired_at + std::chrono::nanoseconds(1'000'000'000 / 60);
    VideoFrame payload;
    payload.captureTimestampNs = SteadyTimeNs(encoded.acquired_at);
    payload.presentationTimestampNs = SteadyTimeNs(encoded.encoded_at);
    if (payload.captureTimestampNs == 0 || payload.presentationTimestampNs == 0) {
      EndSession("video pipeline returned incomplete timestamps");
      return;
    }
    const std::uint64_t capture_timestamp_ns = payload.captureTimestampNs;
    const std::uint64_t presentation_timestamp_ns = payload.presentationTimestampNs;
    payload.accessUnit.swap(encoded.bytes);
    Frame frame;
    frame.header = FrameHeader{encoded.is_idr ? paddrawboard::protocol::kVideoFlagIdr : 0,
                               video_sequence_.fetch_add(1), MessageType::VideoFrame};
    frame.payload = std::move(payload);
    const FrameIoResult started = pending_frame.Start(frame);
    std::get<VideoFrame>(frame.payload).accessUnit.swap(encoded.bytes);
    if (started.status != IoStatus::kOk) {
      EndSession(started.error.empty() ? "cannot serialize video frame" : started.error);
      return;
    }
    pending_sequence = frame.header.sequence;
    pending_capture_timestamp_ns = capture_timestamp_ns;
    pending_presentation_timestamp_ns = presentation_timestamp_ns;
  }
}

void DesktopServer::EndSession(std::string reason) noexcept {
  ClearPendingClockSync();
  if (!session_active_.exchange(false)) {
    telemetry_.Write(TelemetryJson::SessionEnd("pre-active: " + reason));
    return;
  }
  telemetry_.Write(TelemetryJson::SessionEnd(reason));
  const std::string reconnect_reason = reason;
  generation_.fetch_add(1);
  SOCKET control = INVALID_SOCKET;
  SOCKET video = INVALID_SOCKET;
  SOCKET input = INVALID_SOCKET;
  {
    std::scoped_lock lock(socket_mutex_);
    control = control_socket_;
    video = video_socket_;
    input = input_socket_;
    control_socket_ = INVALID_SOCKET;
    video_socket_ = INVALID_SOCKET;
    input_socket_ = INVALID_SOCKET;
  }
  if (control != INVALID_SOCKET) {
    std::scoped_lock write_lock(control_write_mutex_);
    (void)WriteDisconnect(control, control_sequence_.fetch_add(1), 10, reason);
  }
  ShutdownAndClose(control);
  ShutdownAndClose(video);
  ShutdownAndClose(input);
  input_.OnDisconnect();
  current_client_capabilities_.store(0, std::memory_order_release);
  if (running_.load()) {
    adb::AdbSession* session = nullptr;
    {
      std::scoped_lock lock(mutex_);
      session = adb_session_.get();
    }
    if (session != nullptr) session->NotifyChannelLoss(reconnect_reason);
  }
  {
    std::scoped_lock lock(video_mutex_);
    video_.Stop();
  }
  {
    std::scoped_lock lock(mutex_);
    active_pointer_keys_.clear();
    status_.active_pointers = 0;
    status_.phase = running_.load() ? ServerPhase::kAwaitingClient : ServerPhase::kStopped;
    status_.control_connected = false;
    status_.video_connected = false;
    status_.input_connected = false;
    status_.capture_started = false;
    UpdateReleaseReadinessLocked();
    status_.diagnostic = std::move(reason);
  }
  const ServerStatus final_status = Status();
  telemetry_.Write(TelemetryJson::Soak(
      SteadyNowNs(), 0, final_status.active_pointers, final_status.crash_count,
      final_status.stream_resets, std::nullopt));
}

void DesktopServer::ClearPendingClockSync() noexcept {
  std::scoped_lock lock(clock_sync_mutex_);
  pending_clock_sync_ = {};
}

void DesktopServer::RecordPendingClockSync(std::uint64_t device_send_ns,
                                            std::uint64_t host_receive_ns,
                                            std::uint64_t host_send_ns,
                                            std::uint64_t generation,
                                            std::uint64_t issued_at_ns) {
  std::scoped_lock lock(clock_sync_mutex_);
  pending_clock_sync_ = PendingClockSync{
      true, false, device_send_ns, host_receive_ns, host_send_ns,
      generation, issued_at_ns};
}

std::string DesktopServer::ConsumeClockSyncComplete(
    const ClockSyncComplete& completion, std::uint64_t generation,
    std::uint64_t now_ns) {
  std::scoped_lock lock(clock_sync_mutex_);
  if (!pending_clock_sync_.active || pending_clock_sync_.generation != generation) {
    return "mismatched or no pending response";
  }
  if (now_ns >= pending_clock_sync_.issued_at_ns &&
      now_ns - pending_clock_sync_.issued_at_ns > 10'000'000'000ULL) {
    pending_clock_sync_.active = false;
    return "stale response";
  }
  const bool matches = completion.deviceSendNs == pending_clock_sync_.device_send_ns &&
                       completion.hostReceiveNs == pending_clock_sync_.host_receive_ns &&
                       completion.hostSendNs == pending_clock_sync_.host_send_ns;
  if (!matches) return "mismatched response";
  if (pending_clock_sync_.consumed) return "duplicate response";
  if (completion.deviceReceiveNs < completion.deviceSendNs ||
      completion.hostSendNs < completion.hostReceiveNs ||
      completion.deviceReceiveNs - completion.deviceSendNs <
          completion.hostSendNs - completion.hostReceiveNs) {
    return "invalid NTP timestamp ordering";
  }
  pending_clock_sync_.consumed = true;
  return {};
}

bool DesktopServer::IsGenerationActive(std::uint64_t generation) const noexcept {
  return running_.load() && session_active_.load() && generation_.load() == generation;
}

void DesktopServer::UpdateDiagnostic(std::string diagnostic) {
  std::scoped_lock lock(mutex_);
  status_.diagnostic = std::move(diagnostic);
}

void DesktopServer::UpdateActivePointers(const InputBatch& batch) {
  std::scoped_lock lock(mutex_);
  for (const auto& sample : batch.samples) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(sample.tool) << 32u) | sample.pointerId;
    if ((sample.contactFlags & paddrawboard::protocol::kContact) != 0 &&
        (sample.contactFlags & paddrawboard::protocol::kCancelled) == 0) {
      active_pointer_keys_.insert(key);
    } else {
      active_pointer_keys_.erase(key);
    }
  }
  status_.active_pointers = static_cast<std::uint32_t>(active_pointer_keys_.size());
}

void DesktopServer::UpdateReleaseReadinessLocked() noexcept {
  const bool client_buttons_confirmed = IsAllButtonCapabilityPresent(
      current_client_capabilities_.load(std::memory_order_acquire));
  status_.release_ready = ComputeReleaseReady(
      status_.input_probe_completed, status_.pen_buttons_confirmed,
      client_buttons_confirmed,
      status_.input_injection_available, status_.capture_started);

  // Keep the diagnostic synchronized with the same evidence used by the
  // release gate.  Before the ADB probe completes, callers retain the more
  // useful connection/capture diagnostic.
  if (!status_.input_probe_completed) return;
  if (!status_.pen_buttons_confirmed) {
    status_.diagnostic =
        "authorized device probe did not confirm all three pen buttons; release readiness blocked";
  } else if (!client_buttons_confirmed) {
    status_.diagnostic =
        "Android client reports unavailable pen buttons; release readiness blocked";
  } else if (!status_.input_injection_available) {
    status_.diagnostic =
        "Windows synthetic pointer injection is unavailable; release readiness blocked";
  } else if (!status_.capture_started) {
    status_.diagnostic =
        "display capture has not started; release readiness blocked";
  } else {
    status_.diagnostic = status_.release_ready
        ? "all client and ADB input capabilities confirmed; release ready"
        : "release readiness blocked";
  }
}

void DesktopServer::OnCapture(const video::CaptureTelemetry& sample) {
  const ServerStatus status = Status();
  telemetry_.Write(TelemetryJson::Capture(
      SteadyNowNs(), sample.sequence,
      static_cast<std::uint64_t>(sample.acquire_duration.count()),
      static_cast<std::uint64_t>(sample.gpu_copy_duration.count()),
      static_cast<std::uint64_t>(sample.queue_age.count()), sample.replaced_frames));
  telemetry_.Write(TelemetryJson::Soak(
      SteadyNowNs(), 0, status.active_pointers, status.crash_count,
      status.stream_resets, std::nullopt));
}

void DesktopServer::OnEncode(const video::EncodeTelemetry& sample) {
  telemetry_.Write(TelemetryJson::Encode(
      SteadyTimeNs(sample.encoded_at), sample.sequence,
      static_cast<std::uint64_t>(sample.conversion_duration.count()),
      static_cast<std::uint64_t>(sample.encode_duration.count()),
      static_cast<std::uint64_t>(sample.capture_to_encode_duration.count()), sample.idr));
}

void DesktopServer::OnStreamResetRequested() {
  std::scoped_lock lock(mutex_);
  ++status_.stream_resets;
  telemetry_.Write(TelemetryJson::Soak(
      SteadyNowNs(), 0, status_.active_pointers, status_.crash_count,
      status_.stream_resets, std::nullopt));
}

void DesktopServer::OnEncoderSelected(const std::wstring& name, bool asynchronous) {
  std::string escaped;
  escaped.reserve(name.size());
  for (const wchar_t character : name) {
    if (character == L'"' || character == L'\\') escaped += '\\';
    escaped += character < 0x80 ? static_cast<char>(character) : '?';
  }
  telemetry_.Write("{\"kind\":\"encoder_selected\",\"name\":\"" + escaped +
                   "\",\"asynchronous\":" +
                   std::string(asynchronous ? "true" : "false") + "}");
}

std::wstring ServerPhaseText(ServerPhase phase) {
  switch (phase) {
    case ServerPhase::kStopped: return L"已停止";
    case ServerPhase::kStarting: return L"正在启动";
    case ServerPhase::kAwaitingAdb: return L"等待平板连接";
    case ServerPhase::kAwaitingClient: return L"等待平板客户端";
    case ServerPhase::kActive: return L"运行正常";
    case ServerPhase::kDegraded: return L"部分功能不可用";
  }
  return L"未知状态";
}

}  // namespace pdb::app
