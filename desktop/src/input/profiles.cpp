#include "pdb/input/profiles.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>

namespace pdb::input {
namespace {
std::wstring FileName(std::wstring value) {
  const auto separator = value.find_last_of(L"\\/");
  if (separator != std::wstring::npos) value.erase(0, separator + 1);
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return value;
}
}  // namespace

void ProfileResolver::SetProfiles(std::vector<ApplicationProfile> profiles) {
  for (auto& profile : profiles) profile.executableName = FileName(std::move(profile.executableName));
  profiles_ = std::move(profiles);
}

const ApplicationProfile& ProfileResolver::ResolveForExecutable(const std::wstring& executable) const {
  const std::wstring filename = FileName(executable);
  const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const ApplicationProfile& profile) {
    return profile.executableName == filename;
  });
  return found == profiles_.end() ? defaultProfile_ : *found;
}

const ApplicationProfile& ProfileResolver::ResolveForeground() const {
  const HWND window = GetForegroundWindow();
  DWORD processId{};
  if (window == nullptr || GetWindowThreadProcessId(window, &processId) == 0) return defaultProfile_;
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
  if (process == nullptr) return defaultProfile_;
  std::array<wchar_t, 32768> path{};
  DWORD pathLength = static_cast<DWORD>(path.size());
  const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) != FALSE;
  CloseHandle(process);
  return ok ? ResolveForExecutable(std::wstring{path.data(), pathLength}) : defaultProfile_;
}

ApplicationProfile ProfileResolver::BlenderProfile() {
  ApplicationProfile profile;
  profile.executableName = L"blender.exe";
  profile.buttons[0] = Action{ActionKind::kMouseButton, MouseButton::kMiddle, {}};
  profile.buttons[1] = Action{ActionKind::kMouseButton, MouseButton::kMiddle, {VK_SHIFT}};
  profile.buttons[2] = Action{ActionKind::kMouseButton, MouseButton::kMiddle, {VK_CONTROL}};
  return profile;
}

ApplicationProfile ProfileResolver::DefaultProfile() { return ApplicationProfile{}; }
}  // namespace pdb::input
