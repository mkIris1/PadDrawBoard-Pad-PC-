#pragma once

#include "pdb/input/domain.h"
#include "pdb/input/mapper.h"
#include "pdb/input/palm_guard.h"
#include "pdb/input/profiles.h"
#include "pdb/input/synthetic_injector.h"

#include <array>
#include <mutex>

namespace pdb::input {

// Integration boundary: transport adapters convert their decoded domain values to InputFrame.
class InputSystem final {
 public:
  InputSystem();
  [[nodiscard]] bool Configure(const MapperConfig& mapperConfig);
  void SetPalmRejectionEnabled(bool enabled) noexcept;
  void SetProfiles(std::vector<ApplicationProfile> profiles);
  [[nodiscard]] bool Process(const InputFrame& frame);
  void OnDisconnect() noexcept;
  void OnOrientationChange(const MapperConfig& mapperConfig);
  [[nodiscard]] bool IsInjectionAvailable() const noexcept;

 private:
  void ApplyProfileButtons(const PenSample& sample);
  void ReleaseProfileActions() noexcept;

  mutable std::mutex mutex_;
  CoordinateMapper mapper_;
  PalmGuard palmGuard_;
  ProfileResolver profiles_;
  SyntheticPointerInjector injector_;
  std::array<bool, 3> profileButtons_{};
  std::array<Action, 3> pressedActions_{};
  bool effectiveEraser_{};
  bool touchesSuppressed_{};
  bool palmRejectionEnabled_{true};
};

}  // namespace pdb::input
