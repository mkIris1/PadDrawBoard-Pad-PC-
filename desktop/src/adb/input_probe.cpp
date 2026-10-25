#include "pdb/adb/input_probe.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace pdb::adb {
namespace {

std::string Upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return value;
}

void MarkSupported(CapabilityAvailability* capability) {
  if (*capability != CapabilityAvailability::Inaccessible) {
    *capability = CapabilityAvailability::Supported;
  }
}

void FinalizeCapability(CapabilityAvailability* capability, bool inaccessible) {
  if (*capability == CapabilityAvailability::Unknown) {
    *capability = inaccessible ? CapabilityAvailability::Inaccessible
                               : CapabilityAvailability::Unsupported;
  }
}

bool HasToken(std::string_view text, std::string_view token) {
  std::size_t position = text.find(token);
  while (position != std::string_view::npos) {
    const bool left_boundary = position == 0 ||
                               !std::isalnum(static_cast<unsigned char>(
                                   text[position - 1]));
    const std::size_t end = position + token.size();
    const bool right_boundary = end >= text.size() ||
                                !std::isalnum(static_cast<unsigned char>(text[end]));
    if (left_boundary && right_boundary) return true;
    position = text.find(token, position + 1);
  }
  return false;
}

}  // namespace

void InputProbeParser::ConsumeLine(std::string_view line) {
  const std::string upper = Upper(std::string(line));
  if (upper.find("PERMISSION DENIED") != std::string::npos ||
      upper.find("NOT PERMITTED") != std::string::npos ||
      upper.find("INACCESSIBLE") != std::string::npos ||
      upper.find("COULDN'T OPEN") != std::string::npos ||
      upper.find("FAILED TO OPEN") != std::string::npos) {
    result_.permission_denied = true;
    result_.diagnostic_lines.emplace_back(line);
    return;
  }

  if (upper.find("ADD DEVICE") != std::string::npos ||
      upper.find("EVENTS:") != std::string::npos ||
      upper.find("EV_ABS") != std::string::npos) {
    result_.raw_events_accessible = true;
  }
  if (upper.find("ABS_PRESSURE") != std::string::npos) {
    MarkSupported(&result_.capabilities.pressure);
  }
  if (upper.find("ABS_DISTANCE") != std::string::npos) {
    MarkSupported(&result_.capabilities.distance);
    MarkSupported(&result_.capabilities.hover);
  }
  if (upper.find("ABS_TILT_X") != std::string::npos ||
      upper.find("ABS_TILT_Y") != std::string::npos) {
    MarkSupported(&result_.capabilities.tilt);
  }
  if (upper.find("BTN_TOUCH") != std::string::npos ||
      upper.find("ABS_MT_POSITION_X") != std::string::npos) {
    MarkSupported(&result_.capabilities.touch);
  }
  if (HasToken(upper, "BTN_STYLUS3")) {
    MarkSupported(&result_.capabilities.buttons[2]);
  }
  if (HasToken(upper, "BTN_STYLUS2")) {
    MarkSupported(&result_.capabilities.buttons[1]);
  }
  if (HasToken(upper, "BTN_STYLUS")) {
    MarkSupported(&result_.capabilities.buttons[0]);
  }
}

InputProbeResult InputProbeParser::Finish(bool command_succeeded,
                                           bool cancelled) const {
  InputProbeResult result = result_;
  result.command_succeeded = command_succeeded;
  result.cancelled = cancelled;
  const bool inaccessible = result.permission_denied;
  if (inaccessible) {
    result.capabilities.pressure = CapabilityAvailability::Inaccessible;
    result.capabilities.hover = CapabilityAvailability::Inaccessible;
    result.capabilities.tilt = CapabilityAvailability::Inaccessible;
    result.capabilities.distance = CapabilityAvailability::Inaccessible;
    result.capabilities.touch = CapabilityAvailability::Inaccessible;
    for (auto& button : result.capabilities.buttons) {
      button = CapabilityAvailability::Inaccessible;
    }
    if (result.permission_denied) result.raw_events_accessible = false;
    return result;
  }
  FinalizeCapability(&result.capabilities.pressure, inaccessible);
  FinalizeCapability(&result.capabilities.hover, inaccessible);
  FinalizeCapability(&result.capabilities.tilt, inaccessible);
  FinalizeCapability(&result.capabilities.distance, inaccessible);
  FinalizeCapability(&result.capabilities.touch, inaccessible);
  for (auto& button : result.capabilities.buttons) {
    FinalizeCapability(&button, inaccessible);
  }
  return result;
}

std::optional<ParsedInputEvent> InputProbeParser::ParseEventLine(
    std::string_view line) {
  static const std::regex pattern(
      R"(^\[\s*([0-9]+)\.([0-9]+)\s*\]\s+(\S+):\s+(\S+)\s+(\S+)\s+(-?[0-9a-fA-F]+)\s*$)");
  std::cmatch match;
  const std::string copy(line);
  if (!std::regex_match(copy.c_str(), match, pattern)) return std::nullopt;
  try {
    ParsedInputEvent event;
    const std::string seconds(match[1].str());
    const std::string fraction(match[2].str());
    std::string normalized_fraction = fraction.substr(0, 6);
    while (normalized_fraction.size() < 6) normalized_fraction.push_back('0');
    event.timestamp_microseconds = std::stoll(seconds) * 1'000'000 +
                                   std::stoll(normalized_fraction);
    event.device = match[3].str();
    event.event_type = match[4].str();
    event.event_code = match[5].str();
    const std::string value = match[6].str();
    const bool negative = !value.empty() && value.front() == '-';
    const std::string digits = negative ? value.substr(1) : value;
    event.value = std::stoll(digits, nullptr, 16);
    if (negative) event.value = -event.value;
    return event;
  } catch (...) {
    return std::nullopt;
  }
}

InputProbeResult InputProbe::Probe(std::string_view serial,
                                   const InputProbeOptions& options,
                                   std::stop_token stop_token) const {
  InputProbeParser parser;
  const ProcessResult process = client_.Run(BuildFeatureProbeArguments(serial),
                                            stop_token, ProcessOptions{
                                                options.feature_timeout, true});
  auto consume = [&parser](std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t end = text.find('\n', start);
      parser.ConsumeLine(text.substr(start, end == std::string_view::npos
                                              ? std::string_view::npos
                                              : end - start));
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  };
  consume(process.stdout_text);
  consume(process.stderr_text);
  return parser.Finish(process.succeeded(), process.cancelled);
}

std::vector<std::string> InputProbe::BuildFeatureProbeArguments(
    std::string_view serial) {
  return {"-s", std::string(serial), "shell", "getevent", "-lp"};
}

std::vector<std::string> InputProbe::BuildEventStreamArguments(
    std::string_view serial) {
  return {"-s", std::string(serial), "shell", "getevent", "-lt"};
}

}  // namespace pdb::adb
