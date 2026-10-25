#pragma once

#include "pdb/video/types.h"

#include <mutex>

namespace pdb::video {

// Normalizes a desktop-duplication copy so it can be used as a
// VideoProcessorInputView source without introducing CPU-side copies.
[[nodiscard]] D3D11_TEXTURE2D_DESC NormalizePrivateCopyTextureDesc(
    const D3D11_TEXTURE2D_DESC& source_desc) noexcept;

[[nodiscard]] bool ShouldReusePrivateCopyTexture(
    const D3D11_TEXTURE2D_DESC& cached_desc,
    const D3D11_TEXTURE2D_DESC& source_desc) noexcept;

enum class CaptureResult { kFrameAvailable, kTimeout, kRecovered, kFatalError };

// Uses a private default-usage copy of the duplication surface, so the caller
// may hold a frame while the duplication API is immediately released.
class DesktopDuplicationCapture final {
 public:
  DesktopDuplicationCapture() = default;
  ~DesktopDuplicationCapture();
  DesktopDuplicationCapture(const DesktopDuplicationCapture&) = delete;
  DesktopDuplicationCapture& operator=(const DesktopDuplicationCapture&) = delete;

  [[nodiscard]] HRESULT Open(const MonitorInfo& monitor);
  void Close();
  [[nodiscard]] CaptureResult AcquireLatest(CapturedFrame* frame, DWORD timeout_ms,
                                            HRESULT* failure = nullptr);
  [[nodiscard]] Size current_size() const noexcept;
  [[nodiscard]] ComPtr<ID3D11Device> device() const;
  [[nodiscard]] MonitorId monitor_id() const noexcept { return monitor_.id; }

 private:
  [[nodiscard]] HRESULT OpenLocked(const MonitorInfo& monitor);
  [[nodiscard]] HRESULT RecoverLocked();
  [[nodiscard]] HRESULT CreateCopyTextureLocked(const D3D11_TEXTURE2D_DESC& source_desc,
                                                 ComPtr<ID3D11Texture2D>* texture);

  mutable std::mutex mutex_;
  MonitorInfo monitor_{};
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIOutput1> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11Texture2D> private_copy_texture_;
  D3D11_TEXTURE2D_DESC private_copy_desc_{};
  bool private_copy_desc_valid_{};
  Size size_{};
  std::uint64_t sequence_{};
};

}  // namespace pdb::video
