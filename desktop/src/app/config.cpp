#include "pdb/app/config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace pdb::app {
namespace {

struct JsonValue {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind{Kind::kNull};
  bool boolean{};
  std::uint64_t number{};
  std::string string;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue, std::less<>> object;
};

class JsonParser final {
 public:
  explicit JsonParser(std::string_view input) : current_(input.data()), end_(input.data() + input.size()) {}

  [[nodiscard]] bool Parse(JsonValue* value, std::string* error) {
    if (value == nullptr) return Fail(error, "null JSON result");
    SkipWhitespace();
    if (!ParseValue(value, error)) return false;
    SkipWhitespace();
    return current_ == end_ || Fail(error, "trailing JSON data");
  }

 private:
  [[nodiscard]] bool ParseValue(JsonValue* value, std::string* error) {
    SkipWhitespace();
    if (current_ == end_) return Fail(error, "unexpected end of JSON");
    switch (*current_) {
      case '{': return ParseObject(value, error);
      case '[': return ParseArray(value, error);
      case '"':
        value->kind = JsonValue::Kind::kString;
        return ParseString(&value->string, error);
      case 't': return ParseLiteral("true", JsonValue::Kind::kBool, true, value, error);
      case 'f': return ParseLiteral("false", JsonValue::Kind::kBool, false, value, error);
      case 'n': return ParseLiteral("null", JsonValue::Kind::kNull, false, value, error);
      default: return ParseNumber(value, error);
    }
  }

  [[nodiscard]] bool ParseObject(JsonValue* value, std::string* error) {
    ++current_;
    value->kind = JsonValue::Kind::kObject;
    value->object.clear();
    SkipWhitespace();
    if (Consume('}')) return true;
    while (true) {
      std::string key;
      if (!ParseString(&key, error) || !Consume(':')) return Fail(error, "invalid JSON object");
      JsonValue member;
      if (!ParseValue(&member, error)) return false;
      if (!value->object.emplace(std::move(key), std::move(member)).second) {
        return Fail(error, "duplicate JSON key");
      }
      SkipWhitespace();
      if (Consume('}')) return true;
      if (!Consume(',')) return Fail(error, "unterminated JSON object");
      SkipWhitespace();
    }
  }

  [[nodiscard]] bool ParseArray(JsonValue* value, std::string* error) {
    ++current_;
    value->kind = JsonValue::Kind::kArray;
    value->array.clear();
    SkipWhitespace();
    if (Consume(']')) return true;
    while (true) {
      JsonValue item;
      if (!ParseValue(&item, error)) return false;
      value->array.push_back(std::move(item));
      SkipWhitespace();
      if (Consume(']')) return true;
      if (!Consume(',')) return Fail(error, "unterminated JSON array");
      SkipWhitespace();
    }
  }

  [[nodiscard]] bool ParseString(std::string* output, std::string* error) {
    if (output == nullptr || !Consume('"')) return Fail(error, "expected JSON string");
    output->clear();
    while (current_ != end_) {
      const char value = *current_++;
      if (value == '"') return true;
      if (static_cast<unsigned char>(value) < 0x20) return Fail(error, "control character in JSON string");
      if (value != '\\') {
        output->push_back(value);
        continue;
      }
      if (current_ == end_) return Fail(error, "truncated JSON escape");
      switch (*current_++) {
        case '"': output->push_back('"'); break;
        case '\\': output->push_back('\\'); break;
        case '/': output->push_back('/'); break;
        case 'b': output->push_back('\b'); break;
        case 'f': output->push_back('\f'); break;
        case 'n': output->push_back('\n'); break;
        case 'r': output->push_back('\r'); break;
        case 't': output->push_back('\t'); break;
        default: return Fail(error, "unsupported JSON escape");
      }
    }
    return Fail(error, "unterminated JSON string");
  }

  [[nodiscard]] bool ParseNumber(JsonValue* value, std::string* error) {
    const char* start = current_;
    if (current_ != end_ && *current_ == '-') return Fail(error, "negative configuration number");
    while (current_ != end_ && std::isdigit(static_cast<unsigned char>(*current_)) != 0) ++current_;
    if (start == current_) return Fail(error, "invalid JSON value");
    std::uint64_t number{};
    const auto [pointer, result] = std::from_chars(start, current_, number);
    if (result != std::errc{} || pointer != current_) return Fail(error, "invalid JSON number");
    value->kind = JsonValue::Kind::kNumber;
    value->number = number;
    return true;
  }

  [[nodiscard]] bool ParseLiteral(const char* literal, JsonValue::Kind kind, bool boolean,
                                  JsonValue* value, std::string* error) {
    const std::size_t length = std::char_traits<char>::length(literal);
    if (static_cast<std::size_t>(end_ - current_) < length ||
        std::string_view(current_, length) != literal) return Fail(error, "invalid JSON literal");
    current_ += length;
    value->kind = kind;
    value->boolean = boolean;
    return true;
  }

  void SkipWhitespace() {
    while (current_ != end_ && std::isspace(static_cast<unsigned char>(*current_)) != 0) ++current_;
  }
  [[nodiscard]] bool Consume(char expected) {
    SkipWhitespace();
    if (current_ == end_ || *current_ != expected) return false;
    ++current_;
    return true;
  }
  static bool Fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  }

  const char* current_;
  const char* end_;
};

const JsonValue* Member(const JsonValue& object, std::string_view name) {
  if (object.kind != JsonValue::Kind::kObject) return nullptr;
  const auto it = object.object.find(name);
  return it == object.object.end() ? nullptr : &it->second;
}

bool RequireNumber(const JsonValue& object, std::string_view name, std::uint64_t* output,
                   std::string* error) {
  const JsonValue* member = Member(object, name);
  if (member == nullptr || member->kind != JsonValue::Kind::kNumber) {
    if (error != nullptr) *error = "missing numeric configuration field: " + std::string(name);
    return false;
  }
  *output = member->number;
  return true;
}

bool RequireBool(const JsonValue& object, std::string_view name, bool* output, std::string* error) {
  const JsonValue* member = Member(object, name);
  if (member == nullptr || member->kind != JsonValue::Kind::kBool) {
    if (error != nullptr) *error = "missing boolean configuration field: " + std::string(name);
    return false;
  }
  *output = member->boolean;
  return true;
}

bool RequireString(const JsonValue& object, std::string_view name, std::string* output,
                   std::string* error) {
  const JsonValue* member = Member(object, name);
  if (member == nullptr || member->kind != JsonValue::Kind::kString) {
    if (error != nullptr) *error = "missing string configuration field: " + std::string(name);
    return false;
  }
  *output = member->string;
  return true;
}

std::optional<std::wstring> Utf8ToWide(std::string_view text) {
  if (text.empty()) return std::wstring{};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return std::nullopt;
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          output.data(), size) != size) return std::nullopt;
  return output;
}

std::optional<std::string> WideToUtf8(std::wstring_view text) {
  if (text.empty()) return std::string{};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::nullopt;
  std::string output(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          output.data(), size, nullptr, nullptr) != size) return std::nullopt;
  return output;
}

const char* ActionKindName(input::ActionKind kind) {
  switch (kind) {
    case input::ActionKind::kDisabled: return "disabled";
    case input::ActionKind::kMouseButton: return "mouse";
    case input::ActionKind::kKeyChord: return "keys";
    case input::ActionKind::kEraser: return "eraser";
  }
  return "disabled";
}

const char* MouseButtonName(input::MouseButton button) {
  switch (button) {
    case input::MouseButton::kLeft: return "left";
    case input::MouseButton::kMiddle: return "middle";
    case input::MouseButton::kRight: return "right";
    case input::MouseButton::kX1: return "x1";
    case input::MouseButton::kX2: return "x2";
  }
  return "middle";
}

std::optional<input::ActionKind> ParseActionKind(std::string_view name) {
  if (name == "disabled") return input::ActionKind::kDisabled;
  if (name == "mouse") return input::ActionKind::kMouseButton;
  if (name == "keys") return input::ActionKind::kKeyChord;
  if (name == "eraser") return input::ActionKind::kEraser;
  return std::nullopt;
}

std::optional<input::MouseButton> ParseMouseButton(std::string_view name) {
  if (name == "left") return input::MouseButton::kLeft;
  if (name == "middle") return input::MouseButton::kMiddle;
  if (name == "right") return input::MouseButton::kRight;
  if (name == "x1") return input::MouseButton::kX1;
  if (name == "x2") return input::MouseButton::kX2;
  return std::nullopt;
}

void EscapeJson(std::string_view value, std::ostream& output) {
  output << '"';
  for (const char character : value) {
    switch (character) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default: output << character; break;
    }
  }
  output << '"';
}

bool ParseAction(const JsonValue& value, input::Action* action, std::string* error) {
  std::string kind;
  std::string mouse;
  if (action == nullptr || !RequireString(value, "kind", &kind, error) ||
      !RequireString(value, "mouseButton", &mouse, error)) return false;
  const auto parsed_kind = ParseActionKind(kind);
  const auto parsed_mouse = ParseMouseButton(mouse);
  const JsonValue* keys = Member(value, "keys");
  if (!parsed_kind || !parsed_mouse || keys == nullptr || keys->kind != JsonValue::Kind::kArray ||
      keys->array.size() > 8) {
    if (error != nullptr) *error = "invalid profile button action";
    return false;
  }
  input::Action parsed;
  parsed.kind = *parsed_kind;
  parsed.mouseButton = *parsed_mouse;
  for (const JsonValue& key : keys->array) {
    if (key.kind != JsonValue::Kind::kNumber || key.number > std::numeric_limits<std::uint16_t>::max()) {
      if (error != nullptr) *error = "invalid profile virtual key";
      return false;
    }
    parsed.virtualKeys.push_back(static_cast<std::uint16_t>(key.number));
  }
  *action = std::move(parsed);
  return true;
}

bool ParseConfig(const JsonValue& root, AppConfig* config, std::string* error) {
  if (config == nullptr || root.kind != JsonValue::Kind::kObject) {
    if (error != nullptr) *error = "configuration root must be an object";
    return false;
  }
  std::uint64_t schema{};
  std::uint64_t bitrate{};
  std::string monitor;
  bool palm{};
  if (!RequireNumber(root, "schemaVersion", &schema, error) ||
      !RequireString(root, "selectedMonitor", &monitor, error) ||
      !RequireNumber(root, "bitrateMbps", &bitrate, error) ||
      !RequireBool(root, "palmGuard", &palm, error)) return false;
  const JsonValue* profiles = Member(root, "profiles");
  if (schema != kConfigSchemaVersion || bitrate > std::numeric_limits<std::uint32_t>::max() ||
      profiles == nullptr || profiles->kind != JsonValue::Kind::kArray || profiles->array.size() > 32) {
    if (error != nullptr) *error = "unsupported or invalid configuration schema";
    return false;
  }
  const auto wide_monitor = Utf8ToWide(monitor);
  if (!wide_monitor) {
    if (error != nullptr) *error = "selected monitor is not valid UTF-8";
    return false;
  }
  AppConfig parsed;
  parsed.selected_monitor_id = *wide_monitor;
  parsed.bitrate_mbps = static_cast<std::uint32_t>(bitrate);
  parsed.palm_guard_enabled = palm;
  parsed.profiles.clear();
  for (const JsonValue& profile : profiles->array) {
    std::string executable;
    const JsonValue* buttons = Member(profile, "buttons");
    if (!RequireString(profile, "executable", &executable, error) || buttons == nullptr ||
        buttons->kind != JsonValue::Kind::kArray || buttons->array.size() != 3) return false;
    const auto wide_executable = Utf8ToWide(executable);
    if (!wide_executable) {
      if (error != nullptr) *error = "profile executable is not valid UTF-8";
      return false;
    }
    input::ApplicationProfile parsed_profile;
    parsed_profile.executableName = *wide_executable;
    for (std::size_t index = 0; index < parsed_profile.buttons.size(); ++index) {
      if (!ParseAction(buttons->array[index], &parsed_profile.buttons[index], error)) return false;
    }
    parsed.profiles.push_back(std::move(parsed_profile));
  }
  if (!ConfigStore::Validate(parsed, error)) return false;
  *config = std::move(parsed);
  return true;
}

std::string SerializeConfig(const AppConfig& config) {
  std::ostringstream output;
  const auto monitor = WideToUtf8(config.selected_monitor_id).value_or("");
  output << "{\n  \"schemaVersion\": " << config.schema_version << ",\n  \"selectedMonitor\": ";
  EscapeJson(monitor, output);
  output << ",\n  \"bitrateMbps\": " << config.bitrate_mbps
         << ",\n  \"palmGuard\": " << (config.palm_guard_enabled ? "true" : "false")
         << ",\n  \"profiles\": [";
  for (std::size_t profile_index = 0; profile_index < config.profiles.size(); ++profile_index) {
    const input::ApplicationProfile& profile = config.profiles[profile_index];
    const auto executable = WideToUtf8(profile.executableName).value_or("");
    output << (profile_index == 0 ? "\n    " : ",\n    ") << "{\"executable\": ";
    EscapeJson(executable, output);
    output << ", \"buttons\": [";
    for (std::size_t button_index = 0; button_index < profile.buttons.size(); ++button_index) {
      const input::Action& button = profile.buttons[button_index];
      output << (button_index == 0 ? "" : ", ") << "{\"kind\": \"" << ActionKindName(button.kind)
             << "\", \"mouseButton\": \"" << MouseButtonName(button.mouseButton) << "\", \"keys\": [";
      for (std::size_t key_index = 0; key_index < button.virtualKeys.size(); ++key_index) {
        output << (key_index == 0 ? "" : ", ") << button.virtualKeys[key_index];
      }
      output << "]}";
    }
    output << "]}";
  }
  output << "\n  ]\n}\n";
  return output.str();
}

bool WriteAtomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
  std::error_code directory_error;
  std::filesystem::create_directories(path.parent_path(), directory_error);
  if (directory_error) {
    if (error != nullptr) *error = "cannot create configuration directory: " + directory_error.message();
    return false;
  }
  const std::filesystem::path temporary = path.wstring() + L".tmp." +
      std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error != nullptr) *error = "cannot open temporary configuration file";
    return false;
  }
  bool complete = true;
  std::size_t written{};
  while (written < bytes.size()) {
    const std::size_t remaining = bytes.size() - written;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
    DWORD actual{};
    if (!WriteFile(file, bytes.data() + written, chunk, &actual, nullptr) || actual == 0) {
      complete = false;
      break;
    }
    written += actual;
  }
  if (complete && !FlushFileBuffers(file)) complete = false;
  CloseHandle(file);
  if (!complete || !MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    if (error != nullptr) *error = "atomic configuration replacement failed";
    return false;
  }
  return true;
}

}  // namespace

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path ConfigStore::DefaultPath() {
  const DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
  if (needed > 1) {
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    if (GetEnvironmentVariableW(L"APPDATA", value.data(), needed) == needed - 1) {
      value.resize(needed - 1);
      return std::filesystem::path(value) / L"PadDrawBoard" / L"config.json";
    }
  }
  return std::filesystem::current_path() / L"PadDrawBoard" / L"config.json";
}

ConfigLoadResult ConfigStore::Load() const {
  ConfigLoadResult result;
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    result.diagnostic = "configuration not found; using defaults";
    return result;
  }
  std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (json.size() > 256 * 1024) {
    result.diagnostic = "configuration is too large; using defaults";
    return result;
  }
  JsonValue root;
  std::string error;
  if (!JsonParser(json).Parse(&root, &error) || !ParseConfig(root, &result.config, &error)) {
    result.config = AppConfig{};
    result.diagnostic = "configuration ignored: " + error;
    return result;
  }
  result.loaded_from_disk = true;
  result.diagnostic = "configuration loaded";
  return result;
}

bool ConfigStore::Save(const AppConfig& config, std::string* error) const {
  if (!Validate(config, error)) return false;
  return WriteAtomic(path_, SerializeConfig(config), error);
}

bool ConfigStore::Validate(const AppConfig& config, std::string* error) {
  if (config.schema_version != kConfigSchemaVersion || config.bitrate_mbps < 20 ||
      config.bitrate_mbps > 120 || config.selected_monitor_id.size() > 128 ||
      config.profiles.size() > 32) {
    if (error != nullptr) *error = "configuration values are outside v1 limits";
    return false;
  }
  for (const input::ApplicationProfile& profile : config.profiles) {
    if (profile.executableName.empty() || profile.executableName.size() > MAX_PATH) {
      if (error != nullptr) *error = "invalid profile executable";
      return false;
    }
    for (const input::Action& action : profile.buttons) {
      if (action.virtualKeys.size() > 8) {
        if (error != nullptr) *error = "too many keys in profile action";
        return false;
      }
    }
  }
  return true;
}

}  // namespace pdb::app
