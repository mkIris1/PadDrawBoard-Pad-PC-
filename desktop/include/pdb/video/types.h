#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pdb::video {

using Microsoft::WRL::ComPtr;

// Adopt an API-transferred reference without AddRef. The MinGW WRL shipped
// with some toolchains incorrectly AddRefs in Attach(), unlike Microsoft WRL.
// Use the normal COM out-parameter storage to keep ownership identical on both.
template <class Interface>
void AdoptComReference(ComPtr<Interface>& destination, Interface* owned) noexcept {
  *destination.ReleaseAndGetAddressOf() = owned;
}

using SteadyTime = std::chrono::steady_clock::time_point;

struct Size {
  std::uint32_t width{};
  std::uint32_t height{};

  [[nodiscard]] constexpr bool valid() const noexcept { return width != 0 && height != 0; }
  [[nodiscard]] constexpr bool operator==(const Size&) const noexcept = default;
};

// DisplayConfig's adapter LUID plus target ID remains stable across DXGI output
// re-enumeration and does not depend on a localized monitor name or position.
struct MonitorId {
  LUID adapter_luid{};
  std::uint32_t target_id{};

  [[nodiscard]] std::wstring ToString() const;
  [[nodiscard]] static bool TryParse(const std::wstring& value, MonitorId* result);
  [[nodiscard]] bool operator==(const MonitorId& other) const noexcept {
    return adapter_luid.LowPart == other.adapter_luid.LowPart &&
           adapter_luid.HighPart == other.adapter_luid.HighPart && target_id == other.target_id;
  }
};

struct MonitorInfo {
  MonitorId id;
  std::wstring device_name;
  RECT desktop_rect{};
  bool attached_to_desktop{};
};

struct CapturedFrame {
  ComPtr<ID3D11Texture2D> texture;  // Private GPU copy; safe after ReleaseFrame.
  Size size;
  std::uint64_t sequence{};
  SteadyTime acquired_at{};
};

struct EncodedAccessUnit {
  std::vector<std::uint8_t> bytes;
  std::uint64_t sequence{};
  bool is_idr{};
  // Filled by VideoPipeline from the CapturedFrame associated with sequence.
  // This survives delayed asynchronous MFT output.
  SteadyTime acquired_at{};
  // Set by the encoder when the output sample is actually completed.
  SteadyTime encoded_at{};
};

// Reset reusable encoded output metadata without releasing the access-unit
// allocation. The encoder fills bytes with assign(), so retaining its capacity
// avoids a large allocation/free cycle on every video frame.
inline void ResetEncodedAccessUnit(EncodedAccessUnit& output) noexcept {
  output.bytes.clear();
  output.sequence = 0;
  output.is_idr = false;
  output.acquired_at = {};
  output.encoded_at = {};
}

inline constexpr HRESULT kErrFrameFormatUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x502);

}  // namespace pdb::video
