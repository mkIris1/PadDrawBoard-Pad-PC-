#pragma once

#include <cstddef>
#include <cstdint>

namespace pdb::app {

enum class TrayCommandKind { kNone, kExit, kOpenConfigFolder, kOpenTelemetry,
  kExportTelemetry, kTogglePalmGuard, kBitrate, kMonitor };

struct TrayCommand {
  TrayCommandKind kind{TrayCommandKind::kNone};
  std::uint32_t value{};
};

inline constexpr int kTrayExitCommand = 1001;
inline constexpr int kTrayOpenConfigCommand = 1002;
inline constexpr int kTrayOpenTelemetryCommand = 1003;
inline constexpr int kTrayExportTelemetryCommand = 1004;
inline constexpr int kTrayTogglePalmCommand = 1005;
inline constexpr int kTrayBitrateBaseCommand = 1100;
inline constexpr int kTrayMonitorBaseCommand = 1200;

[[nodiscard]] constexpr int TrayBitrateCommand(std::uint32_t bitrate) noexcept {
  return kTrayBitrateBaseCommand + static_cast<int>(bitrate / 10u);
}

[[nodiscard]] constexpr int TrayMonitorCommand(std::size_t index) noexcept {
  return kTrayMonitorBaseCommand + static_cast<int>(index);
}

[[nodiscard]] constexpr bool IsTrayBitratePreset(std::uint32_t bitrate) noexcept {
  return bitrate == 20 || bitrate == 40 || bitrate == 80 || bitrate == 120;
}

[[nodiscard]] constexpr TrayCommand DecodeTrayCommand(int command) noexcept {
  if (command == kTrayExitCommand) return {TrayCommandKind::kExit, 0};
  if (command == kTrayOpenConfigCommand) return {TrayCommandKind::kOpenConfigFolder, 0};
  if (command == kTrayOpenTelemetryCommand) return {TrayCommandKind::kOpenTelemetry, 0};
  if (command == kTrayExportTelemetryCommand) return {TrayCommandKind::kExportTelemetry, 0};
  if (command == kTrayTogglePalmCommand) return {TrayCommandKind::kTogglePalmGuard, 0};
  if (command >= kTrayBitrateBaseCommand && command < kTrayBitrateBaseCommand + 20) {
    return {TrayCommandKind::kBitrate,
            static_cast<std::uint32_t>(command - kTrayBitrateBaseCommand) * 10u};
  }
  if (command >= kTrayMonitorBaseCommand && command < kTrayMonitorBaseCommand + 256) {
    return {TrayCommandKind::kMonitor,
            static_cast<std::uint32_t>(command - kTrayMonitorBaseCommand)};
  }
  return {};
}

}  // namespace pdb::app
