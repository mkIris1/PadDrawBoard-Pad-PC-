#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pdb::adb {

inline constexpr std::uint16_t kControlPort = 48100;
inline constexpr std::uint16_t kVideoPort = 48101;
inline constexpr std::uint16_t kInputPort = 48102;
inline constexpr std::array<std::uint16_t, 3> kReversePorts{
    kControlPort, kVideoPort, kInputPort};
inline constexpr std::string_view kClientPackage = "io.paddrawboard.client";
inline constexpr std::string_view kClientActivity = ".MainActivity";
inline constexpr std::string_view kSessionTokenIntentExtra =
    "io.paddrawboard.client.extra.SESSION_TOKEN";
inline constexpr std::size_t kSessionTokenBytes = 32;
using SessionToken = std::array<std::uint8_t, kSessionTokenBytes>;

[[nodiscard]] std::optional<SessionToken> GenerateSessionToken();
[[nodiscard]] std::string SessionTokenToHex(const SessionToken& token);
[[nodiscard]] std::optional<SessionToken> ParseSessionTokenHex(
    std::string_view value);

enum class DeviceState {
  Authorized,
  Unauthorized,
  Offline,
  Bootloader,
  Unknown,
};

struct DeviceInfo {
  std::string serial;
  DeviceState state = DeviceState::Unknown;
  std::string state_text;
  std::string attributes;
};

enum class DeviceSelectionStatus {
  Authorized,
  NoDevices,
  Unauthorized,
  Offline,
  MultipleDevices,
  MixedStates,
  UnknownState,
  CommandFailed,
};

struct DeviceListing {
  std::vector<DeviceInfo> devices;
  DeviceSelectionStatus status = DeviceSelectionStatus::NoDevices;
  std::optional<DeviceInfo> selected;
  std::string diagnostics;
};

struct ReverseMapping {
  std::uint16_t host_port = 0;
  std::uint16_t device_port = 0;
};

struct ReverseStatus {
  bool command_succeeded = false;
  std::vector<ReverseMapping> mappings;
  std::string raw_output;
};

struct RttResult {
  bool command_succeeded = false;
  std::chrono::microseconds elapsed{};
  std::string response;
};

struct AdbExecutable {
  std::filesystem::path path;
  std::string platform_tools_version;
  bool bundled = false;
};

struct AdbLocatorOptions {
  std::filesystem::path executable_directory;
  std::vector<std::filesystem::path> bundled_candidates;
  std::filesystem::path fallback_path;
  std::string required_bundled_version = "36.0.2";
};

struct AdbLocatorResult {
  std::optional<AdbExecutable> executable;
  std::string diagnostics;
};

struct SessionPorts {
  std::array<ReverseMapping, 3> mappings{{
      {kControlPort, kControlPort},
      {kVideoPort, kVideoPort},
      {kInputPort, kInputPort},
  }};
};

enum class SessionState {
  Stopped,
  Starting,
  Connected,
  Reconnecting,
  Stopping,
};

struct SessionSnapshot {
  SessionState state = SessionState::Stopped;
  std::string serial;
  std::string last_error;
  std::chrono::steady_clock::time_point next_retry{};
  std::size_t reconnect_attempt = 0;
};

enum class CapabilityAvailability {
  Unknown,
  Supported,
  Unsupported,
  Inaccessible,
};

struct InputCapabilities {
  CapabilityAvailability pressure = CapabilityAvailability::Unknown;
  CapabilityAvailability hover = CapabilityAvailability::Unknown;
  CapabilityAvailability tilt = CapabilityAvailability::Unknown;
  CapabilityAvailability distance = CapabilityAvailability::Unknown;
  CapabilityAvailability touch = CapabilityAvailability::Unknown;
  std::array<CapabilityAvailability, 3> buttons{
      CapabilityAvailability::Unknown, CapabilityAvailability::Unknown,
      CapabilityAvailability::Unknown};

  [[nodiscard]] bool all_buttons_observable() const noexcept {
    for (const auto button : buttons) {
      if (button != CapabilityAvailability::Supported) {
        return false;
      }
    }
    return true;
  }
};

struct InputProbeResult {
  bool command_succeeded = false;
  bool cancelled = false;
  bool permission_denied = false;
  bool raw_events_accessible = false;
  InputCapabilities capabilities;
  std::vector<std::string> diagnostic_lines;
};

}  // namespace pdb::adb
