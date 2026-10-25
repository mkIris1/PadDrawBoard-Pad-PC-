#include "pdb/adb/backoff.h"
#include "pdb/adb/client.h"
#include "pdb/adb/input_probe.h"
#include "pdb/adb/loopback.h"
#include "pdb/adb/process.h"
#include "pdb/adb/session.h"

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <mutex>
#include <string>
#include <map>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace pdb::adb;

ProcessResult Success(std::string output = {}) {
  ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.stdout_text = std::move(output);
  return result;
}

class FakeRunner final : public IProcessRunner {
 public:
  struct Call {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
  };

  std::vector<Call> calls;
  std::string devices_output;
  std::string probe_output;
  bool fail_devices = false;
  bool fail_launch = false;
  bool block_launch = false;
  bool release_launch = false;
  bool launch_entered = false;
  std::mutex launch_mutex;
  std::condition_variable launch_condition;
  std::map<std::filesystem::path, ProcessResult> responses;

  ProcessResult Run(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments,
                    const ProcessOptions&, std::stop_token) override {
    calls.push_back({executable, arguments});
    if (arguments == std::vector<std::string>{"version"}) {
      const auto found = responses.find(executable);
      if (found != responses.end()) return found->second;
    }
    if (!arguments.empty() && arguments[0] == "devices") {
      if (fail_devices) {
        ProcessResult result = Success();
        result.exit_code = 1;
        result.stderr_text = "device listing failed";
        return result;
      }
      return Success(devices_output);
    }
    if (arguments.size() >= 4 && arguments[2] == "shell" &&
        arguments[3] == "getevent") {
      return Success(probe_output);
    }
    if (arguments.size() >= 5 && arguments[2] == "shell" &&
        arguments[3] == "am" && arguments[4] == "start") {
      if (block_launch) {
        std::unique_lock lock(launch_mutex);
        launch_entered = true;
        launch_condition.notify_all();
        launch_condition.wait(lock, [this] { return release_launch; });
      }
      if (fail_launch) {
        ProcessResult result = Success();
        result.exit_code = 1;
        result.stderr_text = "launch failed";
        return result;
      }
    }
    return Success();
  }

  void WaitForLaunch() {
    std::unique_lock lock(launch_mutex);
    launch_condition.wait(lock, [this] { return launch_entered; });
  }

  void ReleaseLaunch() {
    {
      std::scoped_lock lock(launch_mutex);
      release_launch = true;
    }
    launch_condition.notify_all();
  }
};

std::filesystem::path MakeFakeAdb(const std::string& name) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream file(path, std::ios::binary);
  file << "fake adb";
  return path;
}

void TestAdbLocatorVersionLock() {
  FakeRunner runner;
  const auto matching = MakeFakeAdb("pdb-adb-matching.exe");
  const auto old_version = MakeFakeAdb("pdb-adb-old.exe");
  const auto newer_version = MakeFakeAdb("pdb-adb-newer.exe");
  const auto malformed = MakeFakeAdb("pdb-adb-malformed.exe");
  const auto fallback = MakeFakeAdb("pdb-adb-fallback.exe");
  runner.responses[matching] = Success(
      "Android Debug Bridge version 1.0.41\nVersion 36.0.2-14143358\n");
  runner.responses[old_version] = Success(
      "Android Debug Bridge version 1.0.32\nVersion 1.0.32\n");
  runner.responses[newer_version] = Success("Version 36.0.3-15000000\n");
  runner.responses[malformed] = Success("Version 36.0.2-not-a-build\n");
  runner.responses[fallback] = Success(
      "Android Debug Bridge version 1.0.41\nVersion 36.0.2-14143358\n");

  AdbLocator locator(runner);
  AdbLocatorOptions options;
  options.bundled_candidates = {matching};
  auto result = locator.LocateWithDiagnostics(options);
  assert(result.executable.has_value());
  assert(result.executable->path == matching);
  assert(result.executable->bundled);

  for (const auto& rejected : {old_version, newer_version, malformed}) {
    options.bundled_candidates = {rejected};
    result = locator.LocateWithDiagnostics(options);
    assert(!result.executable.has_value());
    assert(result.diagnostics.find(rejected.string()) != std::string::npos);
  }

  options.bundled_candidates.clear();
  options.fallback_path = fallback;
  result = locator.LocateWithDiagnostics(options);
  assert(result.executable.has_value());
  assert(result.executable->path == fallback);
  assert(!result.executable->bundled);

  options.fallback_path = old_version;
  result = locator.LocateWithDiagnostics(options);
  assert(!result.executable.has_value());
  assert(result.diagnostics.find("reported version 1.0.32") !=
         std::string::npos);

  for (const auto& path : {matching, old_version, newer_version, malformed,
                           fallback}) {
    std::filesystem::remove(path);
  }
}

class FakeClock final : public IClock {
 public:
  std::chrono::steady_clock::time_point value{};
  std::chrono::steady_clock::time_point Now() const override { return value; }
  void Advance(std::chrono::milliseconds duration) { value += duration; }
};

void TestDeviceParsing() {
  const DeviceListing authorized = ParseDevicesOutput(
      "List of devices attached\nABC123\tdevice product:foo model:Pad_7\n");
  assert(authorized.status == DeviceSelectionStatus::Authorized);
  assert(authorized.selected.has_value());
  assert(authorized.selected->serial == "ABC123");
  assert(authorized.selected->attributes == "product:foo model:Pad_7");

  assert(ParseDevicesOutput("tablet\tunauthorized\n").status ==
         DeviceSelectionStatus::Unauthorized);
  assert(ParseDevicesOutput("tablet\toffline\n").status ==
         DeviceSelectionStatus::Offline);
  assert(ParseDevicesOutput("a\tdevice\nb\tdevice\n").status ==
         DeviceSelectionStatus::MultipleDevices);
  assert(ParseDevicesOutput("\nList of devices attached\n").status ==
         DeviceSelectionStatus::NoDevices);

  const DeviceListing physical_with_emulator = ParseDevicesOutput(
      "List of devices attached\n"
      "emulator-5554\tdevice product:sdk_gphone\n"
      "2186949\tdevice product:foo model:Pad_7\n");
  assert(physical_with_emulator.devices.size() == 2);
  assert(physical_with_emulator.status == DeviceSelectionStatus::Authorized);
  assert(physical_with_emulator.selected.has_value());
  assert(physical_with_emulator.selected->serial == "2186949");

  const DeviceListing two_physical_with_emulator = ParseDevicesOutput(
      "emulator-5554\tdevice\n"
      "tablet-a\tdevice\n"
      "tablet-b\tdevice\n");
  assert(two_physical_with_emulator.devices.size() == 3);
  assert(two_physical_with_emulator.status ==
         DeviceSelectionStatus::MultipleDevices);

  const DeviceListing emulator_only = ParseDevicesOutput(
      "List of devices attached\n"
      "emulator-5554\tdevice\n"
      "emulator-5556\toffline\n");
  assert(emulator_only.devices.size() == 2);
  assert(emulator_only.status == DeviceSelectionStatus::NoDevices);
  assert(!emulator_only.selected.has_value());
  assert(emulator_only.diagnostics.find("emulator") != std::string::npos);

  const DeviceListing unauthorized_physical_with_emulator = ParseDevicesOutput(
      "emulator-5554\tdevice\n"
      "2186949\tunauthorized\n");
  assert(unauthorized_physical_with_emulator.status ==
         DeviceSelectionStatus::Unauthorized);
  assert(unauthorized_physical_with_emulator.devices.size() == 2);

  const DeviceListing offline_physical_with_emulator = ParseDevicesOutput(
      "emulator-5554\tdevice\n"
      "2186949\toffline\n");
  assert(offline_physical_with_emulator.status ==
         DeviceSelectionStatus::Offline);
}

void TestCommandConstruction() {
  assert(AdbClient::BuildReverseArguments("ABC", {48100, 48100}) ==
         std::vector<std::string>({"-s", "ABC", "reverse", "tcp:48100",
                                   "tcp:48100"}));
  const std::string token(64, 'a');
  const auto launch = AdbClient::BuildLaunchArguments("ABC", token);
  assert(launch ==
         std::vector<std::string>({"-s", "ABC", "shell", "am", "start", "-S",
                                   "-n", "io.paddrawboard.client/.MainActivity",
                                   "--es", std::string(kSessionTokenIntentExtra),
                                   token}));
  assert(launch[5] == "-S");
  assert(launch[6] == "-n");
  assert(launch[8] == "--es");
  assert(launch[9] == std::string(kSessionTokenIntentExtra));
  assert(launch[10] == token);
  for (const auto& argument : launch) {
    assert(argument.find('&') == std::string::npos);
    assert(argument.find('|') == std::string::npos);
    assert(argument.find(';') == std::string::npos);
  }
  assert(InputProbe::BuildFeatureProbeArguments("ABC") ==
         std::vector<std::string>({"-s", "ABC", "shell", "getevent", "-lp"}));
  assert(InputProbe::BuildEventStreamArguments("ABC") ==
         std::vector<std::string>({"-s", "ABC", "shell", "getevent", "-lt"}));
  const std::wstring command = BuildWindowsCommandLine(
      L"C:\\Program Files\\PadDrawBoard\\adb.exe", {"-s", "A&B", "shell"});
  assert(command.find(L"PadDrawBoard\\adb.exe") != std::wstring::npos);
  assert(command.find(L"A&B") != std::wstring::npos);
}

void TestSessionTokenEncoding() {
  SessionToken token{};
  for (std::size_t index = 0; index < token.size(); ++index) {
    token[index] = static_cast<std::uint8_t>(index * 7u + 3u);
  }
  const std::string encoded = SessionTokenToHex(token);
  assert(encoded.size() == 64);
  assert(ParseSessionTokenHex(encoded) == token);
  assert(ParseSessionTokenHex(std::string(63, 'a')) == std::nullopt);
  assert(ParseSessionTokenHex(std::string(64, 'g')) == std::nullopt);
  assert(ParseSessionTokenHex(encoded + "0") == std::nullopt);
}

void TestBackoff() {
  ReconnectBackoff backoff;
  assert(backoff.delay_for_attempt(0) == std::chrono::milliseconds(250));
  assert(backoff.delay_for_attempt(1) == std::chrono::milliseconds(500));
  assert(backoff.delay_for_attempt(2) == std::chrono::milliseconds(1000));
  assert(backoff.delay_for_attempt(20) == std::chrono::seconds(8));
  backoff.record_failure();
  backoff.record_failure();
  assert(backoff.attempts() == 2);
  assert(backoff.next_delay() == std::chrono::milliseconds(500));
  backoff.reset();
  assert(backoff.attempts() == 0);
}

void TestInputProbeParsing() {
  InputProbeParser parser;
  parser.ConsumeLine("add device 1: /dev/input/event4");
  parser.ConsumeLine("  ABS (0003): ABS_PRESSURE ABS_DISTANCE ABS_TILT_X ABS_TILT_Y");
  parser.ConsumeLine("  KEY (0001): BTN_TOUCH BTN_STYLUS BTN_STYLUS2 BTN_STYLUS3");
  parser.ConsumeLine("  ABS (0003): ABS_MT_POSITION_X");
  const InputProbeResult result = parser.Finish(true, false);
  assert(result.raw_events_accessible);
  assert(result.capabilities.pressure == CapabilityAvailability::Supported);
  assert(result.capabilities.hover == CapabilityAvailability::Supported);
  assert(result.capabilities.tilt == CapabilityAvailability::Supported);
  assert(result.capabilities.touch == CapabilityAvailability::Supported);
  assert(result.capabilities.all_buttons_observable());

  InputProbeParser denied;
  denied.ConsumeLine("getevent: Permission denied");
  const InputProbeResult denied_result = denied.Finish(false, false);
  assert(denied_result.permission_denied);
  assert(denied_result.capabilities.buttons[0] ==
         CapabilityAvailability::Inaccessible);
  assert(!denied_result.capabilities.all_buttons_observable());

  const auto event = InputProbeParser::ParseEventLine(
      "[  12.345678] /dev/input/event4: EV_ABS ABS_PRESSURE 0000002a");
  assert(event.has_value());
  assert(event->timestamp_microseconds == 12'345'678);
  assert(event->value == 42);
}

void TestProbeAndSession() {
  FakeRunner runner;
  runner.devices_output = "List of devices attached\nABC\tdevice\n";
  runner.probe_output =
      "add device 1: /dev/input/event4\n"
      " ABS_PRESSURE ABS_DISTANCE ABS_TILT_X ABS_TILT_Y\n"
      " BTN_TOUCH BTN_STYLUS BTN_STYLUS2 BTN_STYLUS3\n";
  AdbClient client("adb.exe", runner);
  const InputProbeResult probe = InputProbe(client).Probe("ABC");
  assert(probe.command_succeeded);
  assert(probe.capabilities.all_buttons_observable());

  FakeClock clock;
  runner.devices_output = "List of devices attached\n";
  AdbSession session(client, clock);
  assert(!session.Start());
  assert(session.Snapshot().state == SessionState::Reconnecting);
  assert(session.Snapshot().reconnect_attempt == 1);
  session.Tick();
  assert(session.Snapshot().state == SessionState::Reconnecting);

  runner.devices_output = "List of devices attached\nABC\tdevice\n";
  clock.Advance(std::chrono::milliseconds(250));
  session.Tick();
  assert(session.connected());
  std::string first_token;
  for (const auto& call : runner.calls) {
    if (call.arguments.size() >= 10 && call.arguments[2] == "shell" &&
        call.arguments[3] == "am" && call.arguments[4] == "start") {
      first_token = call.arguments.back();
    }
  }
  assert(first_token.size() == 64);
  assert(ParseSessionTokenHex(first_token).has_value());

  session.NotifyChannelLoss("input channel closed");
  assert(session.Snapshot().state == SessionState::Reconnecting);
  assert(!session.ValidateSessionToken(
      std::span<const std::uint8_t>(ParseSessionTokenHex(first_token)->data(),
                                     kSessionTokenBytes)));
  clock.Advance(std::chrono::milliseconds(250));
  session.Tick();
  assert(session.connected());
  std::string second_token;
  for (const auto& call : runner.calls) {
    if (call.arguments.size() >= 10 && call.arguments[2] == "shell" &&
        call.arguments[3] == "am" && call.arguments[4] == "start") {
      second_token = call.arguments.back();
    }
  }
  assert(second_token.size() == 64);
  assert(second_token != first_token);
  session.Stop();
  assert(session.Snapshot().state == SessionState::Stopped);
  assert(!session.connected());
}

SessionToken TokenFromLaunch(const FakeRunner& runner, std::size_t occurrence = 0) {
  std::size_t found = 0;
  for (const auto& call : runner.calls) {
    if (call.arguments.size() >= 10 && call.arguments[2] == "shell" &&
        call.arguments[3] == "am" && call.arguments[4] == "start") {
      const auto token = ParseSessionTokenHex(call.arguments.back());
      if (token.has_value() && found++ == occurrence) return *token;
    }
  }
  assert(false);
  return {};
}

void TestSessionAuthenticationDuringLaunchAndRevocation() {
  FakeClock clock;
  FakeRunner runner;
  runner.devices_output = "List of devices attached\nABC\tdevice\n";
  runner.block_launch = true;
  AdbClient client("adb.exe", runner);

  AdbSession session(client, clock);
  bool start_result = false;
  std::thread starter([&] { start_result = session.Start(); });
  runner.WaitForLaunch();
  const SessionToken launch_token = TokenFromLaunch(runner);
  assert(session.Snapshot().state == SessionState::Starting);
  assert(session.ValidateSessionToken(launch_token));

  session.Stop();
  assert(!session.ValidateSessionToken(launch_token));
  runner.ReleaseLaunch();
  starter.join();
  assert(!start_result);
  assert(session.Snapshot().state == SessionState::Stopped);

  FakeRunner failed_runner;
  failed_runner.devices_output = "List of devices attached\nABC\tdevice\n";
  failed_runner.block_launch = true;
  failed_runner.fail_launch = true;
  AdbClient failed_client("adb.exe", failed_runner);
  AdbSession failed_session(failed_client, clock);
  bool failed_start_result = true;
  std::thread failed_starter([&] {
    failed_start_result = failed_session.Start();
  });
  failed_runner.WaitForLaunch();
  const SessionToken failed_token = TokenFromLaunch(failed_runner);
  assert(failed_session.ValidateSessionToken(failed_token));
  failed_runner.ReleaseLaunch();
  failed_starter.join();
  assert(!failed_start_result);
  assert(failed_session.Snapshot().state == SessionState::Reconnecting);
  assert(!failed_session.ValidateSessionToken(failed_token));
}

void TestExclusiveLoopbackBinding() {
  LoopbackListener first;
  assert(first.Open(0));
  assert(first.is_open());

  sockaddr_in address{};
  int address_length = sizeof(address);
  assert(getsockname(first.socket(), reinterpret_cast<sockaddr*>(&address),
                     &address_length) == 0);
  const auto port = ntohs(address.sin_port);
  assert(port != 0);

  LoopbackListener second;
  std::string error;
  assert(!second.Open(port, 8, &error));
  assert(!second.is_open());
  assert(!error.empty());
  assert(first.is_open());
}

#ifdef _WIN32
std::filesystem::path CurrentTestExecutable() {
  std::array<wchar_t, 32'768> path{};
  const DWORD length = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  assert(length != 0 && length < path.size());
  return std::filesystem::path(path.data(), path.data() + length);
}

void TestProcessRunnerRestrictsInheritedHandles() {
  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE sentinel = CreateEventW(&security_attributes, TRUE, FALSE, nullptr);
  assert(sentinel != nullptr);

  Win32ProcessRunner runner;
  ProcessOptions options;
  options.timeout = std::chrono::seconds(2);
  const auto result = runner.Run(
      CurrentTestExecutable(),
      {"--pdb-probe-inherited-handle",
       std::to_string(reinterpret_cast<std::uintptr_t>(sentinel))},
      options, std::stop_token{});
  CloseHandle(sentinel);

  assert(result.succeeded());
  assert(result.stdout_text.find("probe-stdout") != std::string::npos);
  assert(result.stderr_text.find("probe-stderr") != std::string::npos);
}

void TestProcessRunnerDoesNotWaitForInheritedPipeWriters() {
  wchar_t com_spec_buffer[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableW(
      L"ComSpec", com_spec_buffer, static_cast<DWORD>(std::size(com_spec_buffer)));
  assert(length != 0 && length < std::size(com_spec_buffer));

  Win32ProcessRunner runner;
  ProcessOptions options;
  options.timeout = std::chrono::seconds(2);
  const auto started_at = std::chrono::steady_clock::now();
  const ProcessResult result = runner.Run(
      com_spec_buffer,
      {"/d", "/s", "/c",
       "echo runner-stdout & echo runner-stderr 1>&2 & start /b cmd /c timeout /t 3 /nobreak"},
      options, std::stop_token{});
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  assert(result.started);
  assert(result.succeeded());
  assert(result.stdout_text.find("runner-stdout") != std::string::npos);
  assert(result.stderr_text.find("runner-stderr") != std::string::npos);
  // The grandchild keeps the inherited pipe write ends open for about three
  // seconds. The runner must finish when cmd.exe exits, without waiting EOF.
  assert(elapsed < std::chrono::seconds(2));
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 3 && std::strcmp(argv[1], "--pdb-probe-inherited-handle") == 0) {
    const auto raw_handle = std::stoull(argv[2]);
    const HANDLE candidate = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(raw_handle));
    DWORD handle_flags = 0;
    const bool inherited = GetHandleInformation(candidate, &handle_flags) != FALSE;
    std::cout << "probe-stdout\n";
    std::cerr << "probe-stderr\n";
    return inherited ? 1 : 0;
  }
#endif
  TestAdbLocatorVersionLock();
  TestDeviceParsing();
  TestCommandConstruction();
  TestSessionTokenEncoding();
  TestBackoff();
  TestInputProbeParsing();
  TestProbeAndSession();
  TestSessionAuthenticationDuringLaunchAndRevocation();
  TestExclusiveLoopbackBinding();
#ifdef _WIN32
  TestProcessRunnerRestrictsInheritedHandles();
  TestProcessRunnerDoesNotWaitForInheritedPipeWriters();
#endif
  std::cout << "pdb_adb_tests: all tests passed\n";
  return 0;
}
