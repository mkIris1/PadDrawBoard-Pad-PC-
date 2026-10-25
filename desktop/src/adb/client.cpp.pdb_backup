#include "pdb/adb/client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pdb::adb {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

std::optional<std::uint8_t> HexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<std::uint8_t>(value - 'A' + 10);
  }
  return std::nullopt;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

DeviceState ParseDeviceState(std::string_view state) {
  const std::string lower = Lower(std::string(state));
  if (lower == "device") return DeviceState::Authorized;
  if (lower == "unauthorized") return DeviceState::Unauthorized;
  if (lower == "offline") return DeviceState::Offline;
  if (lower == "bootloader") return DeviceState::Bootloader;
  return DeviceState::Unknown;
}

std::string JoinError(const ProcessResult& result) {
  if (!result.stderr_text.empty()) return result.stderr_text;
  if (!result.stdout_text.empty()) return result.stdout_text;
  if (result.cancelled) return "ADB command cancelled";
  if (result.timed_out) return "ADB command timed out";
  if (result.win32_error != 0) {
    return "ADB process launch failed: " + std::to_string(result.win32_error);
  }
  return "ADB command exited with code " + std::to_string(result.exit_code);
}

bool IsPortToken(std::string_view token, std::uint16_t* port) {
  constexpr std::string_view prefix = "tcp:";
  if (!token.starts_with(prefix)) return false;
  const std::string number(token.substr(prefix.size()));
  if (number.empty()) return false;
  try {
    const unsigned long parsed = std::stoul(number);
    if (parsed > 65535) return false;
    *port = static_cast<std::uint16_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

std::string MakeComponent() {
  return std::string(kClientPackage) + "/" + std::string(kClientActivity);
}

bool IsDigits(std::string_view value) {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isdigit(character) != 0;
  });
}

bool IsStandardAdbEmulator(std::string_view serial) {
  return serial.starts_with("emulator-");
}

bool IsVersionBase(std::string_view value) {
  const std::size_t first_dot = value.find('.');
  if (first_dot == std::string_view::npos) return false;
  const std::size_t second_dot = value.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos ||
      value.find('.', second_dot + 1) != std::string_view::npos) {
    return false;
  }
  return IsDigits(value.substr(0, first_dot)) &&
         IsDigits(value.substr(first_dot + 1, second_dot - first_dot - 1)) &&
         IsDigits(value.substr(second_dot + 1));
}

std::string LocatorFailure(const std::filesystem::path& path,
                           std::string_view reason) {
  return "Rejected ADB candidate '" + path.string() + "': " +
         std::string(reason) + ".";
}

}  // namespace

std::string SessionTokenToHex(const SessionToken& token) {
  std::string result;
  result.reserve(kSessionTokenBytes * 2);
  for (const std::uint8_t byte : token) {
    result.push_back(kHexDigits[byte >> 4u]);
    result.push_back(kHexDigits[byte & 0x0fu]);
  }
  return result;
}

std::optional<SessionToken> ParseSessionTokenHex(std::string_view value) {
  if (value.size() != kSessionTokenBytes * 2) return std::nullopt;
  SessionToken token{};
  for (std::size_t index = 0; index < kSessionTokenBytes; ++index) {
    const auto high = HexNibble(value[index * 2]);
    const auto low = HexNibble(value[index * 2 + 1]);
    if (!high || !low) return std::nullopt;
    token[index] = static_cast<std::uint8_t>((*high << 4u) | *low);
  }
  return token;
}

AdbClient::AdbClient(std::filesystem::path executable, IProcessRunner& runner,
                     AdbCommandOptions options)
    : executable_(std::move(executable)), runner_(runner), options_(options) {}

ProcessResult AdbClient::Run(const std::vector<std::string>& arguments,
                             std::stop_token stop_token,
                             std::optional<ProcessOptions> override_options) const {
  return runner_.Run(executable_, arguments,
                     override_options.value_or(options_.process), stop_token);
}

ProcessResult AdbClient::Version(std::stop_token stop_token) const {
  return Run({"version"}, stop_token);
}

DeviceListing AdbClient::ListDevices(std::stop_token stop_token) const {
  const ProcessResult process = Run(BuildListDevicesArguments(), stop_token);
  if (!process.started || !process.succeeded()) {
    DeviceListing listing;
    listing.status = DeviceSelectionStatus::CommandFailed;
    listing.diagnostics = JoinError(process);
    return listing;
  }
  return ParseDevicesOutput(process.stdout_text);
}

DeviceListing AdbClient::SelectSingleAuthorizedDevice(
    std::stop_token stop_token) const {
  return ListDevices(stop_token);
}

ProcessResult AdbClient::InstallReverseMappings(std::string_view serial,
                                                const SessionPorts& ports,
                                                std::stop_token stop_token) const {
  ProcessResult last_result;
  for (const ReverseMapping mapping : ports.mappings) {
    last_result = Run(BuildReverseArguments(serial, mapping), stop_token);
    if (!last_result.succeeded()) return last_result;
  }
  return last_result;
}

ProcessResult AdbClient::RemoveReverseMappings(std::string_view serial,
                                               const SessionPorts& ports,
                                               std::stop_token stop_token) const {
  ProcessResult last_result;
  for (const ReverseMapping mapping : ports.mappings) {
    last_result = Run(BuildReverseRemoveArguments(serial, mapping.host_port),
                      stop_token);
  }
  return last_result;
}

ProcessResult AdbClient::LaunchClient(std::string_view serial,
                                      std::string_view session_token_hex,
                                      std::stop_token stop_token) const {
  return Run(BuildLaunchArguments(serial, session_token_hex), stop_token);
}

ProcessResult AdbClient::StopClient(std::string_view serial,
                                    std::stop_token stop_token) const {
  return Run(BuildStopArguments(serial), stop_token);
}

ReverseStatus AdbClient::QueryReverseStatus(std::string_view serial,
                                             std::stop_token stop_token) const {
  const ProcessResult process = Run(BuildReverseListArguments(serial), stop_token);
  return ParseReverseStatus(process.stdout_text + process.stderr_text,
                            process.succeeded());
}

RttResult AdbClient::MeasureRtt(std::string_view serial,
                                std::stop_token stop_token) const {
  const auto started_at = std::chrono::steady_clock::now();
  const ProcessResult process = Run(BuildRttArguments(serial), stop_token);
  RttResult result;
  result.command_succeeded = process.succeeded();
  result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started_at);
  result.response = process.stdout_text;
  return result;
}

std::vector<std::string> AdbClient::BuildListDevicesArguments() {
  return {"devices", "-l"};
}

std::vector<std::string> AdbClient::BuildReverseArguments(
    std::string_view serial, ReverseMapping mapping) {
  return {"-s", std::string(serial), "reverse",
          "tcp:" + std::to_string(mapping.host_port),
          "tcp:" + std::to_string(mapping.device_port)};
}

std::vector<std::string> AdbClient::BuildReverseListArguments(
    std::string_view serial) {
  return {"-s", std::string(serial), "reverse", "--list"};
}

std::vector<std::string> AdbClient::BuildReverseRemoveArguments(
    std::string_view serial, std::uint16_t host_port) {
  return {"-s", std::string(serial), "reverse", "--remove",
          "tcp:" + std::to_string(host_port)};
}

std::vector<std::string> AdbClient::BuildLaunchArguments(
    std::string_view serial, std::string_view session_token_hex) {
  return {"-s", std::string(serial), "shell", "am", "start", "-S", "-n",
          MakeComponent(), "--es", std::string(kSessionTokenIntentExtra),
          std::string(session_token_hex)};
}

std::vector<std::string> AdbClient::BuildStopArguments(
    std::string_view serial) {
  return {"-s", std::string(serial), "shell", "am", "force-stop",
          std::string(kClientPackage)};
}

std::vector<std::string> AdbClient::BuildRttArguments(std::string_view serial) {
  // Fixed argv, deliberately without a remote shell expression.
  return {"-s", std::string(serial), "shell", "echo", "pdb_rtt_probe"};
}

DeviceListing ParseDevicesOutput(std::string_view output) {
  DeviceListing listing;
  std::istringstream stream{std::string(output)};
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line == "List of devices attached" ||
        line.starts_with("* daemon")) {
      continue;
    }
    std::istringstream line_stream(line);
    DeviceInfo device;
    if (!(line_stream >> device.serial >> device.state_text)) continue;
    std::getline(line_stream, device.attributes);
    if (!device.attributes.empty() && device.attributes.front() == ' ') {
      device.attributes.erase(device.attributes.begin());
    }
    device.state = ParseDeviceState(device.state_text);
    listing.devices.push_back(std::move(device));
  }

  std::vector<const DeviceInfo*> physical_devices;
  physical_devices.reserve(listing.devices.size());
  for (const DeviceInfo& device : listing.devices) {
    if (!IsStandardAdbEmulator(device.serial)) {
      physical_devices.push_back(&device);
    }
  }

  // Keep every row for diagnostics, but do not let a standard local emulator
  // compete with a physical tablet for the session.
  if (physical_devices.empty()) {
    listing.status = DeviceSelectionStatus::NoDevices;
    if (!listing.devices.empty()) {
      listing.diagnostics =
          "Only standard ADB emulator devices are present; waiting for a "
          "physical device.";
    }
  } else if (physical_devices.size() > 1) {
    listing.status = DeviceSelectionStatus::MultipleDevices;
  } else {
    const DeviceInfo& device = *physical_devices.front();
    switch (device.state) {
      case DeviceState::Authorized:
        listing.status = DeviceSelectionStatus::Authorized;
        listing.selected = device;
        break;
      case DeviceState::Unauthorized:
        listing.status = DeviceSelectionStatus::Unauthorized;
        break;
      case DeviceState::Offline:
        listing.status = DeviceSelectionStatus::Offline;
        break;
      case DeviceState::Unknown:
        listing.status = DeviceSelectionStatus::UnknownState;
        break;
      case DeviceState::Bootloader:
        listing.status = DeviceSelectionStatus::MixedStates;
        break;
    }
  }
  return listing;
}

ReverseStatus ParseReverseStatus(std::string_view output,
                                 bool command_succeeded) {
  ReverseStatus status;
  status.command_succeeded = command_succeeded;
  status.raw_output = std::string(output);
  std::istringstream stream{std::string(output)};
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream line_stream(line);
    std::string token;
    std::vector<std::uint16_t> ports;
    while (line_stream >> token) {
      std::uint16_t port = 0;
      if (IsPortToken(token, &port)) ports.push_back(port);
    }
    if (ports.size() >= 2) {
      status.mappings.push_back({ports[0], ports[1]});
    }
  }
  return status;
}

std::optional<std::string> ExtractPlatformToolsVersion(std::string_view output) {
  constexpr std::string_view marker = "Version ";
  std::size_t line_start = 0;
  while (line_start <= output.size()) {
    std::size_t line_end = output.find('\n', line_start);
    if (line_end == std::string_view::npos) line_end = output.size();
    std::string_view line = output.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with(marker)) {
      const std::string_view token = line.substr(marker.size());
      const std::size_t suffix_separator = token.find('-');
      const std::string_view base =
          token.substr(0, suffix_separator == std::string_view::npos
                              ? token.size()
                              : suffix_separator);
      if (!IsVersionBase(base)) return std::nullopt;
      if (suffix_separator != std::string_view::npos &&
          !IsDigits(token.substr(suffix_separator + 1))) {
        return std::nullopt;
      }
      return std::string(base);
    }
    if (line_end == output.size()) break;
    line_start = line_end + 1;
  }
  return std::nullopt;
}

AdbLocatorResult AdbLocator::LocateWithDiagnostics(
    const AdbLocatorOptions& options) const {
  AdbLocatorResult result;
  std::ostringstream diagnostics;
  std::vector<std::filesystem::path> bundled = options.bundled_candidates;
  if (bundled.empty() && !options.executable_directory.empty()) {
    const auto& root = options.executable_directory;
    bundled = {root / "tools" / "adb" / "36.0.2" / "adb.exe",
               root / "tools" / "adb" / "platform-tools" / "adb.exe",
               root / "adb" / "adb.exe", root / "platform-tools" / "adb.exe"};
  }
  std::unordered_set<std::wstring> seen;
  const auto try_candidate = [&](const std::filesystem::path& path,
                                 bool bundled_candidate) -> bool {
    if (path.empty() || !seen.insert(path.wstring()).second) {
      return false;
    }
    if (!std::filesystem::is_regular_file(path)) {
      diagnostics << LocatorFailure(path, "the candidate is not a regular file")
                  << '\n';
      return false;
    }
    const ProcessResult version = runner_.Run(path, {"version"}, {}, {});
    if (!version.succeeded()) {
      diagnostics << LocatorFailure(path, "the version command failed") << '\n';
      return false;
    }
    const auto parsed = ExtractPlatformToolsVersion(version.stdout_text +
                                                    version.stderr_text);
    if (!parsed) {
      diagnostics << LocatorFailure(
          path, "the output has no valid platform-tools Version line") << '\n';
      return false;
    }
    if (*parsed != options.required_bundled_version) {
      diagnostics << LocatorFailure(
          path, "reported version " + *parsed + " but required " +
                options.required_bundled_version)
                 << '\n';
      return false;
    }
    result.executable = AdbExecutable{path, *parsed, bundled_candidate};
    return true;
  };

  for (const auto& path : bundled) {
    if (try_candidate(path, true)) {
      result.diagnostics = diagnostics.str();
      return result;
    }
  }
  if (!options.fallback_path.empty() && try_candidate(options.fallback_path, false)) {
    result.diagnostics = diagnostics.str();
    return result;
  }
  result.diagnostics = diagnostics.str();
  return result;
}

std::optional<AdbExecutable> AdbLocator::Locate(
    const AdbLocatorOptions& options) const {
  return LocateWithDiagnostics(options).executable;
}

}  // namespace pdb::adb
