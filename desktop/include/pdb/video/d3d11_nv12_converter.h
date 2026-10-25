#pragma once

#include "pdb/video/types.h"

#include <string_view>

namespace pdb::video {

struct Nv12TextureCacheKey {
  Size size{};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};

  [[nodiscard]] constexpr bool operator==(const Nv12TextureCacheKey&) const noexcept = default;
};

// The output texture is reusable only for the explicitly approved asynchronous
// path, with an unchanged texture description and the same D3D11 device.
[[nodiscard]] constexpr bool ShouldReuseNv12OutputTexture(
    bool allow_output_texture_reuse, Nv12TextureCacheKey cached,
    Nv12TextureCacheKey requested, bool same_device) noexcept {
  return allow_output_texture_reuse && same_device && cached == requested;
}

// D3D11 video-processor based scale/BGRA-to-NV12 conversion. This is kept as
// an abstraction because some adapters do not expose NV12 processor output.
class D3D11Nv12Converter final {
 public:
  D3D11Nv12Converter() = default;
  ~D3D11Nv12Converter();
  D3D11Nv12Converter(const D3D11Nv12Converter&) = delete;
  D3D11Nv12Converter& operator=(const D3D11Nv12Converter&) = delete;

  [[nodiscard]] HRESULT Initialize(ID3D11Device* device);
  void Reset();
  // allow_output_texture_reuse is valid only for an asynchronous MFT. When
  // enabled, the caller must not call Convert again until the prior
  // ProcessInput has completed successfully through ProcessOutput.
  [[nodiscard]] HRESULT Convert(ID3D11Texture2D* bgra_source, Size output_size,
                                bool allow_output_texture_reuse,
                                ComPtr<ID3D11Texture2D>* nv12_output);
  [[nodiscard]] std::string_view last_failure_stage() const noexcept {
    return last_failure_stage_;
  }

 private:
  [[nodiscard]] HRESULT Configure(Size input_size, DXGI_FORMAT input_format, Size output_size);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  Size input_size_{};
  DXGI_FORMAT input_format_{DXGI_FORMAT_UNKNOWN};
  Size output_size_{};
  ComPtr<ID3D11Texture2D> cached_output_texture_;
  ComPtr<ID3D11Device> cached_output_device_;
  Nv12TextureCacheKey cached_output_key_{};
  std::string_view last_failure_stage_{"none"};
};

}  // namespace pdb::video
