#pragma once

#include "pdb/input/profiles.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pdb::app {

inline constexpr std::uint32_t kConfigSchemaVersion = 1;

// This is deliberately the complete persisted surface for v1.  Transient
// transport and device state is never written to disk.
struct AppConfig {
  std::uint32_t schema_version{kConfigSchemaVersion};
  std::wstring selected_monitor_id;
  std::uint32_t bitrate_mbps{80};
  bool palm_guard_enabled{true};
  std::vector<input::ApplicationProfile> profiles{input::ProfileResolver::BlenderProfile()};
};

struct ConfigLoadResult {
  AppConfig config{};
  bool loaded_from_disk{};
  std::string diagnostic;
};

class ConfigStore final {
 public:
  explicit ConfigStore(std::filesystem::path path = DefaultPath());

  [[nodiscard]] ConfigLoadResult Load() const;
  [[nodiscard]] bool Save(const AppConfig& config, std::string* error = nullptr) const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  [[nodiscard]] static std::filesystem::path DefaultPath();
  [[nodiscard]] static bool Validate(const AppConfig& config, std::string* error = nullptr);

 private:
  std::filesystem::path path_;
};

}  // namespace pdb::app
