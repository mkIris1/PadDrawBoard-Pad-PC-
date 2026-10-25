#pragma once

#include "pdb/input/domain.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pdb::input {

enum class ActionKind : std::uint8_t { kDisabled, kMouseButton, kKeyChord, kEraser };
enum class MouseButton : std::uint8_t { kLeft, kMiddle, kRight, kX1, kX2 };

struct Action {
  ActionKind kind{ActionKind::kDisabled};
  MouseButton mouseButton{MouseButton::kMiddle};
  // Win32 virtual-key values pressed before a mouse action, or as a chord.
  std::vector<std::uint16_t> virtualKeys;
};

struct ApplicationProfile {
  std::wstring executableName;  // case-insensitive filename, for example blender.exe
  std::array<Action, 3> buttons{};
};

class ProfileResolver final {
 public:
  void SetProfiles(std::vector<ApplicationProfile> profiles);
  [[nodiscard]] const ApplicationProfile& ResolveForExecutable(const std::wstring& executable) const;
  [[nodiscard]] const ApplicationProfile& ResolveForeground() const;
  [[nodiscard]] static ApplicationProfile BlenderProfile();
  [[nodiscard]] static ApplicationProfile DefaultProfile();

 private:
  std::vector<ApplicationProfile> profiles_;
  ApplicationProfile defaultProfile_{DefaultProfile()};
};

}  // namespace pdb::input
