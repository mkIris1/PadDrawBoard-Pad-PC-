#pragma once

#include "pdb/adb/adb_types.h"
#include "pdb/adb/process.h"

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pdb::adb {

struct AdbCommandOptions {
  ProcessOptions process;
};

class AdbClient {
 public:
  AdbClient(std::filesystem::path executable, IProcessRunner& runner,
            AdbCommandOptions options = {});

  [[nodiscard]] const std::filesystem::path& executable() const noexcept {
    return executable_;
  }

  ProcessResult Run(const std::vector<std::string>& arguments,
                    std::stop_token stop_token = {},
                    std::optional<ProcessOptions> override_options =
                        std::nullopt) const;

  ProcessResult Version(std::stop_token stop_token = {}) const;
  DeviceListing ListDevices(std::stop_token stop_token = {}) const;
  DeviceListing SelectSingleAuthorizedDevice(
      std::stop_token stop_token = {}) const;
  ProcessResult InstallReverseMappings(
      std::string_view serial, const SessionPorts& ports = {},
      std::stop_token stop_token = {}) const;
  ProcessResult RemoveReverseMappings(std::string_view serial,
                                       const SessionPorts& ports = {},
                                       std::stop_token stop_token = {}) const;
  ProcessResult LaunchClient(std::string_view serial,
                             std::string_view session_token_hex,
                             std::stop_token stop_token = {}) const;
  ProcessResult StopClient(std::string_view serial,
                           std::stop_token stop_token = {}) const;
  ReverseStatus QueryReverseStatus(std::string_view serial,
                                    std::stop_token stop_token = {}) const;
  RttResult MeasureRtt(std::string_view serial,
                       std::stop_token stop_token = {}) const;

  [[nodiscard]] static std::vector<std::string> BuildListDevicesArguments();
  [[nodiscard]] static std::vector<std::string> BuildReverseArguments(
      std::string_view serial, ReverseMapping mapping);
  [[nodiscard]] static std::vector<std::string> BuildReverseListArguments(
      std::string_view serial);
  [[nodiscard]] static std::vector<std::string> BuildReverseRemoveArguments(
      std::string_view serial, std::uint16_t host_port);
  [[nodiscard]] static std::vector<std::string> BuildLaunchArguments(
      std::string_view serial, std::string_view session_token_hex);
  [[nodiscard]] static std::vector<std::string> BuildStopArguments(
      std::string_view serial);
  [[nodiscard]] static std::vector<std::string> BuildRttArguments(
      std::string_view serial);

 private:
  std::filesystem::path executable_;
  IProcessRunner& runner_;
  AdbCommandOptions options_;
};

[[nodiscard]] DeviceListing ParseDevicesOutput(std::string_view output);
[[nodiscard]] ReverseStatus ParseReverseStatus(std::string_view output,
                                               bool command_succeeded);
[[nodiscard]] std::optional<std::string> ExtractPlatformToolsVersion(
    std::string_view output);

class AdbLocator {
 public:
  explicit AdbLocator(IProcessRunner& runner) : runner_(runner) {}

  [[nodiscard]] AdbLocatorResult LocateWithDiagnostics(
      const AdbLocatorOptions& options) const;

  [[nodiscard]] std::optional<AdbExecutable> Locate(
      const AdbLocatorOptions& options) const;

 private:
  IProcessRunner& runner_;
};

}  // namespace pdb::adb
