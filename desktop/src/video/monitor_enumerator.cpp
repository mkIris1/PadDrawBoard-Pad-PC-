#include "pdb/video/monitor_enumerator.h"

#include <dxgi1_6.h>

#include <cwchar>
#include <algorithm>

namespace pdb::video {
namespace {

bool SameLuid(const LUID& left, const LUID& right) {
  return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

struct DisplayTarget {
  LUID adapter_luid{};
  UINT32 target_id{};
  WCHAR gdi_name[CCHDEVICENAME]{};
};

HRESULT ActiveDisplayTargets(std::vector<DisplayTarget>* targets) {
  if (targets == nullptr) return E_POINTER;
  targets->clear();
  UINT32 path_count{};
  UINT32 mode_count{};
  LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                              nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  for (UINT32 index = 0; index < path_count; ++index) {
    const auto& path = paths[index];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    DisplayTarget target{};
    target.adapter_luid = path.targetInfo.adapterId;
    target.target_id = path.targetInfo.id;
    constexpr std::size_t kMaxDeviceNameLength = CCHDEVICENAME - 1;
    std::size_t name_length = 0;
    while (name_length < kMaxDeviceNameLength &&
           source.viewGdiDeviceName[name_length] != L'\0') {
      ++name_length;
    }
    std::wmemcpy(target.gdi_name, source.viewGdiDeviceName, name_length);
    target.gdi_name[name_length] = L'\0';
    targets->push_back(target);
  }
  return S_OK;
}

}  // namespace

std::wstring MonitorId::ToString() const {
  return std::to_wstring(adapter_luid.HighPart) + L":" +
         std::to_wstring(adapter_luid.LowPart) + L":" + std::to_wstring(target_id);
}

bool MonitorId::TryParse(const std::wstring& value, MonitorId* result) {
  if (result == nullptr) return false;
  const wchar_t* cursor = value.c_str();
  wchar_t* end = nullptr;
  const long high = std::wcstol(cursor, &end, 10);
  if (end == cursor || *end != L':') return false;
  cursor = end + 1;

  const unsigned long low = std::wcstoul(cursor, &end, 10);
  if (end == cursor || *end != L':') return false;
  cursor = end + 1;

  const unsigned long output = std::wcstoul(cursor, &end, 10);
  if (end == cursor) return false;
  result->adapter_luid.HighPart = high;
  result->adapter_luid.LowPart = low;
  result->target_id = static_cast<std::uint32_t>(output);
  return true;
}

HRESULT MonitorEnumerator::Enumerate(std::vector<MonitorInfo>* monitors) {
  if (monitors == nullptr) return E_POINTER;
  monitors->clear();

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return hr;

  std::vector<DisplayTarget> targets;
  hr = ActiveDisplayTargets(&targets);
  if (FAILED(hr)) return hr;

  for (UINT adapter_index = 0;; ++adapter_index) {
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapter_index, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr)) return hr;

    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (FAILED(adapter->GetDesc1(&adapter_desc))) continue;
    for (UINT output_index = 0;; ++output_index) {
      ComPtr<IDXGIOutput> output;
      hr = adapter->EnumOutputs(output_index, &output);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hr)) return hr;

      DXGI_OUTPUT_DESC output_desc{};
      hr = output->GetDesc(&output_desc);
      if (FAILED(hr)) return hr;
      const auto target = std::find_if(targets.begin(), targets.end(), [&](const DisplayTarget& candidate) {
        return SameLuid(candidate.adapter_luid, adapter_desc.AdapterLuid) &&
               std::wcscmp(candidate.gdi_name, output_desc.DeviceName) == 0;
      });
      // Detached outputs have no active DisplayConfig target and cannot be
      // captured, so they intentionally are not selectable.
      if (target == targets.end()) continue;
      MonitorInfo info{};
      info.id.adapter_luid = adapter_desc.AdapterLuid;
      info.id.target_id = target->target_id;
      info.device_name = output_desc.DeviceName;
      info.desktop_rect = output_desc.DesktopCoordinates;
      info.attached_to_desktop = output_desc.AttachedToDesktop != FALSE;
      monitors->push_back(std::move(info));
    }
  }
  return S_OK;
}

HRESULT MonitorEnumerator::Find(const MonitorId& id, MonitorInfo* monitor) {
  if (monitor == nullptr) return E_POINTER;
  std::vector<MonitorInfo> monitors;
  HRESULT hr = Enumerate(&monitors);
  if (FAILED(hr)) return hr;
  for (const auto& candidate : monitors) {
    if (candidate.id == id) {
      *monitor = candidate;
      return S_OK;
    }
  }
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

}  // namespace pdb::video
