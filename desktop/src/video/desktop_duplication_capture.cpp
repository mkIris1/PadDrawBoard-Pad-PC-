#include "pdb/video/desktop_duplication_capture.h"

#include "pdb/video/monitor_enumerator.h"

#include <d3d10.h>
#include <d3d11.h>

#include <cwchar>

namespace pdb::video {

D3D11_TEXTURE2D_DESC NormalizePrivateCopyTextureDesc(
    const D3D11_TEXTURE2D_DESC& source_desc) noexcept {
  D3D11_TEXTURE2D_DESC desc = source_desc;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = 0;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.MiscFlags = 0;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  return desc;
}

bool ShouldReusePrivateCopyTexture(const D3D11_TEXTURE2D_DESC& cached_desc,
                                   const D3D11_TEXTURE2D_DESC& source_desc) noexcept {
  const auto normalized = NormalizePrivateCopyTextureDesc(source_desc);
  return cached_desc.Width == normalized.Width && cached_desc.Height == normalized.Height &&
         cached_desc.MipLevels == normalized.MipLevels &&
         cached_desc.ArraySize == normalized.ArraySize &&
         cached_desc.Format == normalized.Format &&
         cached_desc.SampleDesc.Count == normalized.SampleDesc.Count &&
         cached_desc.SampleDesc.Quality == normalized.SampleDesc.Quality &&
         cached_desc.Usage == normalized.Usage &&
         cached_desc.BindFlags == normalized.BindFlags &&
         cached_desc.CPUAccessFlags == normalized.CPUAccessFlags &&
         cached_desc.MiscFlags == normalized.MiscFlags;
}

DesktopDuplicationCapture::~DesktopDuplicationCapture() { Close(); }

HRESULT DesktopDuplicationCapture::Open(const MonitorInfo& monitor) {
  std::scoped_lock lock(mutex_);
  duplication_.Reset();
  output_.Reset();
  private_copy_texture_.Reset();
  private_copy_desc_ = {};
  private_copy_desc_valid_ = false;
  context_.Reset();
  device_.Reset();
  size_ = {};
  sequence_ = 0;
  return OpenLocked(monitor);
}

HRESULT DesktopDuplicationCapture::OpenLocked(const MonitorInfo& monitor) {
  if (!monitor.attached_to_desktop) return DXGI_ERROR_UNSUPPORTED;
  monitor_ = monitor;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return hr;

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> candidate;
    hr = factory->EnumAdapters1(i, &candidate);
    if (hr == DXGI_ERROR_NOT_FOUND) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (FAILED(hr)) return hr;
    DXGI_ADAPTER_DESC1 description{};
    hr = candidate->GetDesc1(&description);
    if (FAILED(hr)) continue;
    if (description.AdapterLuid.LowPart == monitor.id.adapter_luid.LowPart &&
        description.AdapterLuid.HighPart == monitor.id.adapter_luid.HighPart) {
      adapter = candidate;
      break;
    }
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  D3D_FEATURE_LEVEL actual_feature_level{};
  hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                         D3D11_SDK_VERSION, &device_, &actual_feature_level, &context_);
  if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &device_, &actual_feature_level, &context_);
  }
  if (FAILED(hr)) return hr;

  // Desktop Duplication, the D3D11 video processor, and the asynchronous
  // Media Foundation encoder share this device.  Enable the D3D11 runtime's
  // multithread protection before exposing the device to any of them.
  ComPtr<ID3D10Multithread> multithread;
  hr = device_.As(&multithread);
  if (FAILED(hr)) {
    context_.Reset();
    device_.Reset();
    return hr;
  }
  // SetMultithreadProtected returns the previous protection state, not an
  // HRESULT or success flag.  A false return value is therefore expected
  // when protection was previously disabled.
  (void)multithread->SetMultithreadProtected(TRUE);

  ComPtr<IDXGIOutput> base_output;
  for (UINT output_index = 0;; ++output_index) {
    ComPtr<IDXGIOutput> candidate;
    hr = adapter->EnumOutputs(output_index, &candidate);
    if (hr == DXGI_ERROR_NOT_FOUND) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (FAILED(hr)) return hr;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(candidate->GetDesc(&desc))) continue;
    if (std::wcscmp(desc.DeviceName, monitor.device_name.c_str()) == 0) {
      base_output = std::move(candidate);
      break;
    }
  }
  hr = base_output.As(&output_);
  if (FAILED(hr)) return hr;
  hr = output_->DuplicateOutput(device_.Get(), &duplication_);
  if (FAILED(hr)) return hr;
  return S_OK;
}

void DesktopDuplicationCapture::Close() {
  std::scoped_lock lock(mutex_);
  duplication_.Reset();
  output_.Reset();
  private_copy_texture_.Reset();
  private_copy_desc_ = {};
  private_copy_desc_valid_ = false;
  context_.Reset();
  device_.Reset();
  size_ = {};
  sequence_ = 0;
}

HRESULT DesktopDuplicationCapture::RecoverLocked() {
  MonitorInfo current;
  const HRESULT hr = MonitorEnumerator::Find(monitor_.id, &current);
  if (FAILED(hr)) return hr;
  duplication_.Reset();
  output_.Reset();
  private_copy_texture_.Reset();
  private_copy_desc_ = {};
  private_copy_desc_valid_ = false;
  context_.Reset();
  device_.Reset();
  size_ = {};
  return OpenLocked(current);
}

HRESULT DesktopDuplicationCapture::CreateCopyTextureLocked(const D3D11_TEXTURE2D_DESC& source_desc,
                                                           ComPtr<ID3D11Texture2D>* texture) {
  if (texture == nullptr) return E_POINTER;
  const D3D11_TEXTURE2D_DESC desc = NormalizePrivateCopyTextureDesc(source_desc);
  texture->Reset();
  if (!private_copy_texture_ || !private_copy_desc_valid_ ||
      !ShouldReusePrivateCopyTexture(private_copy_desc_, source_desc)) {
    ComPtr<ID3D11Texture2D> replacement;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &replacement);
    if (FAILED(hr)) return hr;
    private_copy_texture_ = std::move(replacement);
    private_copy_desc_ = desc;
    private_copy_desc_valid_ = true;
  }
  *texture = private_copy_texture_;
  size_ = {desc.Width, desc.Height};
  return S_OK;
}

CaptureResult DesktopDuplicationCapture::AcquireLatest(CapturedFrame* frame, DWORD timeout_ms,
                                                       HRESULT* failure) {
  if (failure) *failure = S_OK;
  if (frame == nullptr) {
    if (failure) *failure = E_POINTER;
    return CaptureResult::kFatalError;
  }
  std::scoped_lock lock(mutex_);
  if (!duplication_) {
    if (failure) *failure = E_HANDLE;
    return CaptureResult::kFatalError;
  }

  DXGI_OUTDUPL_FRAME_INFO frame_info{};
  ComPtr<IDXGIResource> acquired_resource;
  HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &frame_info, &acquired_resource);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CaptureResult::kTimeout;
  if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_SESSION_DISCONNECTED ||
      hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    const HRESULT recover_hr = RecoverLocked();
    if (failure) *failure = FAILED(recover_hr) ? recover_hr : hr;
    return FAILED(recover_hr) ? CaptureResult::kFatalError : CaptureResult::kRecovered;
  }
  if (FAILED(hr)) {
    if (failure) *failure = hr;
    return CaptureResult::kFatalError;
  }

  // ReleaseFrame must occur on every acquired frame, even when conversion fails.
  auto release_frame = [this]() { return duplication_->ReleaseFrame(); };
  ComPtr<ID3D11Texture2D> desktop_texture;
  hr = acquired_resource.As(&desktop_texture);
  if (SUCCEEDED(hr)) {
    D3D11_TEXTURE2D_DESC source_desc{};
    desktop_texture->GetDesc(&source_desc);
    ComPtr<ID3D11Texture2D> private_copy;
    hr = CreateCopyTextureLocked(source_desc, &private_copy);
    if (SUCCEEDED(hr)) {
      context_->CopyResource(private_copy.Get(), desktop_texture.Get());
      // The caller serializes capture/conversion with the asynchronous encoder:
      // no consumer still reads this texture when the next capture overwrites it.
      frame->texture = private_copy;
      frame->size = size_;
      frame->sequence = ++sequence_;
      frame->acquired_at = std::chrono::steady_clock::now();
    }
  }
  const HRESULT release_hr = release_frame();
  if (FAILED(hr)) {
    if (failure) *failure = hr;
    return CaptureResult::kFatalError;
  }
  if (FAILED(release_hr)) {
    if (failure) *failure = release_hr;
    return CaptureResult::kFatalError;
  }
  return CaptureResult::kFrameAvailable;
}

Size DesktopDuplicationCapture::current_size() const noexcept {
  std::scoped_lock lock(mutex_);
  return size_;
}

ComPtr<ID3D11Device> DesktopDuplicationCapture::device() const {
  std::scoped_lock lock(mutex_);
  return device_;
}

}  // namespace pdb::video
