#pragma once

#include "pdb/app/config.h"
#include "pdb/app/input_adapter.h"
#include "pdb/app/telemetry.h"
#include "pdb/adb/loopback.h"
#include "pdb/adb/adb_types.h"
#include "pdb/input/input_system.h"
#include "pdb/video/telemetry.h"
#include "pdb/video/video_pipeline.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace pdb::adb {
class AdbClient;
class AdbSession;
class IClock;
class IProcessRunner;
}  // namespace pdb::adb

namespace pdb::app {

enum class ServerPhase { kStopped, kStarting, kAwaitingAdb, kAwaitingClient, kActive, kDegraded };

struct ServerStatus {
  ServerPhase phase{ServerPhase::kStopped};
  bool control_connected{};
  bool video_connected{};
  bool input_connected{};
  bool input_injection_available{};
  bool input_probe_completed{};
  bool pen_buttons_confirmed{};
  bool release_ready{};
  bool capture_started{};
  std::uint64_t session_id{};
  std::uint64_t video_frames_sent{};
  std::uint64_t stream_resets{};
  std::uint32_t client_rtt_us{};
  std::uint32_t client_video_latency_us{};
  std::uint32_t active_pointers{};
  std::uint64_t crash_count{};
  std::string input_probe_diagnostic;
  std::string diagnostic;
};

struct ServerOptions {
  AppConfig config{};
  ConfigStore config_store{};
  std::filesystem::path executable_directory;
};

// The desktop transport authority. It accepts only loopback peers established
// by the one ADB session and tears down all synthetic input on every session
// transition.
class DesktopServer final : private video::VideoTelemetrySink {
 public:
  explicit DesktopServer(ServerOptions options);
  ~DesktopServer();

  DesktopServer(const DesktopServer&) = delete;
  DesktopServer& operator=(const DesktopServer&) = delete;

  [[nodiscard]] bool Start();
  void Stop() noexcept;
  // Fast path used on exit: restores the tablet settings without performing the
  // full teardown, so the display mode flips back immediately.
  void RestoreTabletForExit() noexcept;
  [[nodiscard]] bool Reconfigure(AppConfig config, std::string* error = nullptr);
  [[nodiscard]] AppConfig CurrentConfig() const;
  [[nodiscard]] ServerStatus Status() const;
  [[nodiscard]] const std::filesystem::path& TelemetryPath() const noexcept {
    return telemetry_.path();
  }
  [[nodiscard]] const std::filesystem::path& ConfigPath() const noexcept {
    return options_.config_store.path();
  }
  [[nodiscard]] bool ExportTelemetry(const std::filesystem::path& destination,
                                     std::string* error = nullptr) const {
    return telemetry_.CopyTo(destination, error);
  }

 private:
  void AcceptLoop(std::stop_token stop_token);
  void AdbLoop(std::stop_token stop_token);
  void ControlLoop(std::stop_token stop_token, std::uint64_t generation);
  void InputLoop(std::stop_token stop_token, std::uint64_t generation);
  void VideoLoop(std::stop_token stop_token, std::uint64_t generation);
  [[nodiscard]] bool EstablishSession(SOCKET control_socket);
  void EndSession(std::string reason) noexcept;
  [[nodiscard]] bool IsGenerationActive(std::uint64_t generation) const noexcept;
  void UpdateDiagnostic(std::string diagnostic);
  void UpdateActivePointers(const paddrawboard::protocol::InputBatch& batch);
  void UpdateReleaseReadinessLocked() noexcept;
  void ClearPendingClockSync() noexcept;
  void RecordPendingClockSync(std::uint64_t device_send_ns,
                              std::uint64_t host_receive_ns,
                              std::uint64_t host_send_ns,
                              std::uint64_t generation,
                              std::uint64_t issued_at_ns);
  [[nodiscard]] std::string ConsumeClockSyncComplete(
      const paddrawboard::protocol::ClockSyncComplete& completion,
      std::uint64_t generation, std::uint64_t now_ns);

  void OnCapture(const video::CaptureTelemetry&) override;
  void OnEncode(const video::EncodeTelemetry&) override;
  void OnStreamResetRequested() override;
  void OnEncoderSelected(const std::wstring& name, bool asynchronous) override;

  ServerOptions options_;
  mutable std::mutex mutex_;
  mutable std::mutex config_mutex_;
  ServerStatus status_;
  input::InputSystem input_;
  InputBatchAdapter input_adapter_;
  std::mutex input_adapter_mutex_;
  video::VideoPipeline video_;
  std::mutex video_mutex_;
  video::MonitorInfo selected_monitor_;
  video::Size configured_video_size_;
  std::uint32_t video_max_longest_edge_{3200};
  std::uint64_t orientation_epoch_{1};
  paddrawboard::protocol::ClientHello hello_;
  // This is the authoritative capability snapshot for the active session.
  // ClientHello is immutable session history; CapabilityChanged may refine it
  // after Android has observed the stylus in use.
  std::atomic<std::uint32_t> current_client_capabilities_{0};
  adb::LoopbackListener control_listener_;
  adb::LoopbackListener video_listener_;
  adb::LoopbackListener input_listener_;
  SOCKET control_socket_{INVALID_SOCKET};
  SOCKET video_socket_{INVALID_SOCKET};
  SOCKET input_socket_{INVALID_SOCKET};
  std::mutex socket_mutex_;
  std::mutex control_write_mutex_;
  std::jthread accept_thread_;
  std::jthread adb_thread_;
  std::jthread control_thread_;
  std::jthread input_thread_;
  std::jthread video_thread_;
  std::unique_ptr<adb::IProcessRunner> process_runner_;
  std::unique_ptr<adb::IClock> clock_;
  std::unique_ptr<adb::AdbClient> adb_client_;
  std::unique_ptr<adb::AdbSession> adb_session_;
  TelemetryWriter telemetry_;
  struct PendingClockSync final {
    bool active{};
    bool consumed{};
    std::uint64_t device_send_ns{};
    std::uint64_t host_receive_ns{};
    std::uint64_t host_send_ns{};
    std::uint64_t generation{};
    std::uint64_t issued_at_ns{};
  };
  mutable std::mutex clock_sync_mutex_;
  PendingClockSync pending_clock_sync_;
  std::set<std::uint64_t> active_pointer_keys_;
  std::atomic<bool> running_{false};
  std::atomic<bool> session_active_{false};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint32_t> control_sequence_{1};
  std::atomic<std::uint32_t> video_sequence_{1};
};

[[nodiscard]] constexpr bool ComputeReleaseReady(
    bool probe_completed, bool probe_buttons_confirmed, bool client_buttons_confirmed,
    bool injection_available, bool capture_started) noexcept {
  return probe_completed && probe_buttons_confirmed && client_buttons_confirmed &&
         injection_available && capture_started;
}

[[nodiscard]] std::wstring ServerPhaseText(ServerPhase phase);

}  // namespace pdb::app
