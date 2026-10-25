#include "pdb/app/config.h"
#include "pdb/app/client_handshake_watchdog.h"
#include "pdb/app/desktop_server.h"
#include "pdb/app/telemetry.h"
#include "pdb/app/tray_commands.h"
#include "pdb/app/session_auth.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>

namespace {

void TestClientHandshakeWatchdog() {
  using namespace std::chrono_literals;
  pdb::app::ClientHandshakeWatchdog watchdog{5s};
  const auto start = std::chrono::steady_clock::time_point{};
  assert(!watchdog.Observe(false, false, start));
  assert(!watchdog.Observe(true, false, start));
  assert(!watchdog.Observe(true, false, start + 4999ms));
  assert(watchdog.Observe(true, false, start + 5s));
  assert(!watchdog.Observe(true, true, start + 6s));
  assert(!watchdog.Observe(true, false, start + 7s));
  assert(!watchdog.Observe(false, false, start + 20s));
}

std::filesystem::path TestDirectory() {
  return std::filesystem::temp_directory_path() /
      (L"PadDrawBoard-config-test-" + std::to_wstring(GetCurrentProcessId()));
}

void TestRoundTripAndAtomicReplacement() {
  const std::filesystem::path directory = TestDirectory();
  const std::filesystem::path path = directory / L"config.json";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  pdb::app::ConfigStore store(path);
  pdb::app::AppConfig config;
  config.selected_monitor_id = L"-1:123:4";
  config.bitrate_mbps = 96;
  config.palm_guard_enabled = false;
  std::string error;
  assert(store.Save(config, &error));
  assert(std::filesystem::exists(path));
  const auto loaded = store.Load();
  assert(loaded.loaded_from_disk);
  assert(loaded.config.selected_monitor_id == config.selected_monitor_id);
  assert(loaded.config.bitrate_mbps == 96);
  assert(!loaded.config.palm_guard_enabled);
  assert(loaded.config.profiles.size() == 1);
  assert(loaded.config.profiles[0].executableName == L"blender.exe");
  std::filesystem::remove_all(directory, ignored);
}

void TestInvalidSchemaFallsBackSafely() {
  const std::filesystem::path directory = TestDirectory();
  const std::filesystem::path path = directory / L"config.json";
  std::filesystem::create_directories(directory);
  {
    std::ofstream output(path, std::ios::binary);
    output << "{\"schemaVersion\": 99}";
  }
  const pdb::app::ConfigStore store(path);
  const auto loaded = store.Load();
  assert(!loaded.loaded_from_disk);
  assert(loaded.config.bitrate_mbps == 80);
  assert(!loaded.diagnostic.empty());
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

void TestTelemetrySchemaAndReadiness() {
  const std::string clock = pdb::app::TelemetryJson::ClockSync(10, 20, 30, 40);
  assert(clock.find("\"kind\":\"clock_sync\"") != std::string::npos);
  assert(clock.find("\"device_receive_ns\":40") != std::string::npos);
  assert(clock.find("\"complete\":true") != std::string::npos);
  const std::string input = pdb::app::TelemetryJson::InputTransport(10, 20, 10, 2);
  assert(input.find("\"kind\":\"input_transport\"") != std::string::npos);
  assert(input.find("\"active_pointers\":2") != std::string::npos);
  const std::string soak = pdb::app::TelemetryJson::Soak(10, 0, 0, 0, 1, 32.0);
  assert(soak.find("\"kind\":\"soak\"") != std::string::npos);
  assert(soak.find("\"queue_depth\":0") != std::string::npos);
  const std::string session_end = pdb::app::TelemetryJson::SessionEnd(
      "video pipeline: \"failed\\retry\"\n\r\t\b\f\x01 HRESULT 0x887A0005");
  assert(session_end ==
         "{\"kind\":\"session_end\",\"reason\":\"video pipeline: \\\"failed\\\\retry\\\"\\n\\r\\t\\b\\f\\u0001 HRESULT 0x887A0005\"}");
  assert(session_end.find("\"kind\":\"session_end\"") != std::string::npos);
  assert(session_end.find("HRESULT 0x887A0005") != std::string::npos);
  assert(!pdb::app::ComputeReleaseReady(false, true, true, true, true));
  assert(!pdb::app::ComputeReleaseReady(true, false, true, true, true));
  assert(!pdb::app::ComputeReleaseReady(true, true, false, true, true));
  assert(pdb::app::ComputeReleaseReady(true, true, true, true, true));
}

void TestTelemetryWriterPersistentStreamAndRotation() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      (L"PadDrawBoard-telemetry-test-" + std::to_wstring(GetCurrentProcessId()));
  const std::filesystem::path path = directory / L"nested" / L"telemetry.jsonl";
  const std::filesystem::path copy = directory / L"copy.jsonl";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);

  {
    pdb::app::TelemetryWriter writer(path);
    writer.Write("first");
    writer.Write("second");

    std::string initial;
    {
      std::ifstream before_rotation(path, std::ios::binary);
      initial.assign(std::istreambuf_iterator<char>(before_rotation), {});
    }
    assert(initial == "first\nsecond\n");
    std::string error;
    assert(writer.CopyTo(copy, &error));
    std::string copied_text;
    {
      std::ifstream copied(copy, std::ios::binary);
      copied_text.assign(std::istreambuf_iterator<char>(copied), {});
    }
    assert(copied_text == initial);

    const std::string large(2'000'000, 'x');
    const std::string rotation_trigger(200'000, 'y');
    writer.Write(large);
    writer.Write(large);
    writer.Write(rotation_trigger);
    writer.Write("after-rotation");

    assert(std::filesystem::exists(
        std::filesystem::path(path.wstring() + L".1")));
    std::string current_text;
    {
      std::ifstream current(path, std::ios::binary);
      current_text.assign(std::istreambuf_iterator<char>(current), {});
    }
    assert(current_text == rotation_trigger + "\nafter-rotation\n");
    writer.Write("after-copy");
    assert(writer.CopyTo(copy, &error));
    std::string copied_latest_text;
    {
      std::ifstream copied_latest(copy, std::ios::binary);
      copied_latest_text.assign(
          std::istreambuf_iterator<char>(copied_latest), {});
    }
    assert(copied_latest_text ==
           rotation_trigger + "\nafter-rotation\nafter-copy\n");
  }
  std::filesystem::remove_all(directory, ignored);
}

void TestDynamicClientCapabilityReadiness() {
  constexpr std::uint32_t required_buttons =
      paddrawboard::protocol::kCapabilityButton1 |
      paddrawboard::protocol::kCapabilityButton2 |
      paddrawboard::protocol::kCapabilityButton3;

  // The initial ClientHello is deliberately incomplete.  A later valid
  // CapabilityChanged is the only event that is allowed to make the client
  // side of the gate pass.
  paddrawboard::protocol::ClientHello hello;
  hello.capabilities = 0;
  std::uint32_t current_capabilities = hello.capabilities;
  const auto client_buttons_confirmed = [&] {
    return (current_capabilities & required_buttons) == required_buttons;
  };
  const auto ready = [&](bool probe_completed, bool probe_buttons_confirmed,
                         bool injection_available, bool capture_started) {
    return pdb::app::ComputeReleaseReady(
        probe_completed, probe_buttons_confirmed, client_buttons_confirmed(),
        injection_available, capture_started);
  };

  assert(!ready(true, true, true, true));

  paddrawboard::protocol::CapabilityChanged changed{required_buttons};
  current_capabilities = changed.capabilities;
  assert(!ready(false, true, true, true));
  assert(!ready(true, false, true, true));
  assert(!ready(true, true, false, true));
  assert(!ready(true, true, true, false));
  assert(ready(true, true, true, true));
}

void TestTrayCommandLogic() {
  assert(pdb::app::DecodeTrayCommand(pdb::app::kTrayExitCommand).kind ==
         pdb::app::TrayCommandKind::kExit);
  assert(pdb::app::DecodeTrayCommand(pdb::app::kTrayOpenConfigCommand).kind ==
         pdb::app::TrayCommandKind::kOpenConfigFolder);
  assert(pdb::app::DecodeTrayCommand(pdb::app::kTrayOpenTelemetryCommand).kind ==
         pdb::app::TrayCommandKind::kOpenTelemetry);
  assert(pdb::app::DecodeTrayCommand(pdb::app::kTrayExportTelemetryCommand).kind ==
         pdb::app::TrayCommandKind::kExportTelemetry);
  assert(pdb::app::DecodeTrayCommand(pdb::app::kTrayTogglePalmCommand).kind ==
         pdb::app::TrayCommandKind::kTogglePalmGuard);
  assert(pdb::app::DecodeTrayCommand(pdb::app::TrayBitrateCommand(80)).value == 80);
  assert(pdb::app::IsTrayBitratePreset(20));
  assert(pdb::app::IsTrayBitratePreset(120));
  assert(!pdb::app::IsTrayBitratePreset(60));
  const auto monitor = pdb::app::DecodeTrayCommand(pdb::app::TrayMonitorCommand(7));
  assert(monitor.kind == pdb::app::TrayCommandKind::kMonitor && monitor.value == 7);
  assert(pdb::app::DecodeTrayCommand(999).kind == pdb::app::TrayCommandKind::kNone);
  assert(pdb::app::ServerPhaseText(pdb::app::ServerPhase::kActive) == L"运行正常");
  assert(pdb::app::ServerPhaseText(pdb::app::ServerPhase::kDegraded) == L"部分功能不可用");
}

void TestSessionAuthPreface() {
  pdb::adb::SessionToken token{};
  for (std::size_t index = 0; index < token.size(); ++index) {
    token[index] = static_cast<std::uint8_t>(index + 1);
  }
  const auto preface = pdb::app::BuildAuthPreface(
      token, pdb::app::AuthChannel::Control);
  assert(preface.size() == pdb::app::kAuthPrefaceBytes);
  assert(pdb::app::ValidateAuthPreface(
      std::span<const std::uint8_t>(preface.data(), preface.size()), token,
      pdb::app::AuthChannel::Control));
  assert(!pdb::app::ValidateAuthPreface(
      std::span<const std::uint8_t>(preface.data(), preface.size()), token,
      pdb::app::AuthChannel::Video));
  auto wrong_token = token;
  wrong_token.back() ^= 1u;
  assert(!pdb::app::ValidateAuthPreface(
      std::span<const std::uint8_t>(preface.data(), preface.size()), wrong_token,
      pdb::app::AuthChannel::Control));
  auto truncated = preface;
  assert(!pdb::app::ValidateAuthPreface(
      std::span<const std::uint8_t>(truncated.data(), truncated.size() - 1),
      token, pdb::app::AuthChannel::Control));
}

}  // namespace

int main() {
  TestClientHandshakeWatchdog();
  TestRoundTripAndAtomicReplacement();
  TestInvalidSchemaFallsBackSafely();
  TestTelemetrySchemaAndReadiness();
  TestTelemetryWriterPersistentStreamAndRotation();
  TestDynamicClientCapabilityReadiness();
  TestTrayCommandLogic();
  TestSessionAuthPreface();
  std::cout << "pdb_app_config_tests: all tests passed\n";
  return 0;
}
