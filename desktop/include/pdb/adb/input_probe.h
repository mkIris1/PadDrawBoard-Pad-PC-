#pragma once

#include "pdb/adb/adb_types.h"
#include "pdb/adb/client.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pdb::adb {

struct ParsedInputEvent {
  std::int64_t timestamp_microseconds = 0;
  std::string device;
  std::string event_type;
  std::string event_code;
  std::int64_t value = 0;
};

class InputProbeParser {
 public:
  void ConsumeLine(std::string_view line);
  [[nodiscard]] InputProbeResult Finish(bool command_succeeded,
                                         bool cancelled) const;

  [[nodiscard]] static std::optional<ParsedInputEvent> ParseEventLine(
      std::string_view line);

 private:
  InputProbeResult result_;
};

struct InputProbeOptions {
  std::chrono::milliseconds feature_timeout{5'000};
};

class InputProbe {
 public:
  explicit InputProbe(const AdbClient& client) : client_(client) {}

  [[nodiscard]] InputProbeResult Probe(std::string_view serial,
                                       const InputProbeOptions& options = {},
                                       std::stop_token stop_token = {}) const;

  [[nodiscard]] static std::vector<std::string> BuildFeatureProbeArguments(
      std::string_view serial);
  [[nodiscard]] static std::vector<std::string> BuildEventStreamArguments(
      std::string_view serial);

 private:
  const AdbClient& client_;
};

}  // namespace pdb::adb
