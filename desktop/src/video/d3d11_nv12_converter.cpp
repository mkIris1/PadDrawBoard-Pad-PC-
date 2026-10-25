#include "pdb/video/d3d11_nv12_converter.h"

namespace pdb::video {

D3D11Nv12Converter::~D3D11Nv12Converter() { Reset(); }

HRESULT D3D11Nv12Converter::Initialize(ID3D11Device* device) {
  last_failure_stage_ = "initialize";
  if (device == nullptr) return E_POINTER;
  Reset();
  device_ = device;
  device_->GetImmediateContext(&context_);
  HRESULT hr = device_.As(&video_device_);
  if (FAILED(hr)) {
    Reset();
    return hr;
  }
  hr = context_.As(&video_context_);
  if (FAILED(hr)) {
    Reset();
    return hr;
  }
  return S_OK;
}

void D3D11Nv12Converter::Reset() {
  cached_output_texture_.Reset();
  cached_output_device_.Reset();
  cached_output_key_ = {};
  processor_.Reset();
  enumerator_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
  input_size_ = {};
  input_format_ = DXGI_FORMAT_UNKNOWN;
  output_size_ = {};
}

HRESULT D3D11Nv12Converter::Configure(Size input_size, DXGI_FORMAT input_format, Size output_size) {
  last_failure_stage_ = "configure_arguments";
  if (!input_size.valid() || !output_size.valid() || (output_size.width & 1) ||
      (output_size.height & 1)) {
    return E_INVALIDARG;
  }
  if (enumerator_ && input_size == input_size_ && input_format == input_format_ &&
      output_size == output_size_) return S_OK;

  processor_.Reset();
  enumerator_.Reset();
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = input_size.width;
  content.InputHeight = input_size.height;
  content.OutputWidth = output_size.width;
  content.OutputHeight = output_size.height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  content.InputFrameRate = {60, 1};
  content.OutputFrameRate = {60, 1};

  last_failure_stage_ = "create_processor_enumerator";
  HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
  if (FAILED(hr)) return hr;
  UINT support{};
  last_failure_stage_ = "check_input_format";
  hr = enumerator_->CheckVideoProcessorFormat(input_format, &support);
  if (FAILED(hr)) return hr;
  if ((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    return kErrFrameFormatUnsupported;
  }
  last_failure_stage_ = "check_output_format";
  hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support);
  if (FAILED(hr)) return hr;
  if ((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    return kErrFrameFormatUnsupported;
  }
  last_failure_stage_ = "create_processor";
  hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
  if (FAILED(hr)) return hr;

  // DXGI Desktop Duplication is sRGB BGRA. Output color space is Rec.709
  // limited-range YUV, matching the default SDR H.264 encoder expectation.
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
  input.RGB_Range = 0;
  input.YCbCr_Matrix = 1;
  input.Nominal_Range = 2;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
  output.Usage = 0;
  output.RGB_Range = 0;
  output.YCbCr_Matrix = 1;
  output.Nominal_Range = 2;
  video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input);
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output);
  input_size_ = input_size;
  input_format_ = input_format;
  output_size_ = output_size;
  return S_OK;
}

HRESULT D3D11Nv12Converter::Convert(ID3D11Texture2D* bgra_source, Size output_size,
                                    bool allow_output_texture_reuse,
                                    ComPtr<ID3D11Texture2D>* nv12_output) {
  last_failure_stage_ = "convert_arguments";
  if (bgra_source == nullptr || nv12_output == nullptr || !device_ || !video_device_) return E_POINTER;
  nv12_output->Reset();
  D3D11_TEXTURE2D_DESC source_desc{};
  bgra_source->GetDesc(&source_desc);
  if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
      source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
    return kErrFrameFormatUnsupported;
  }
  last_failure_stage_ = "configure";
  HRESULT hr = Configure({source_desc.Width, source_desc.Height}, source_desc.Format, output_size);
  if (FAILED(hr)) return hr;

  D3D11_TEXTURE2D_DESC destination{};
  destination.Width = output_size.width;
  destination.Height = output_size.height;
  destination.MipLevels = 1;
  destination.ArraySize = 1;
  destination.Format = DXGI_FORMAT_NV12;
  destination.SampleDesc.Count = 1;
  destination.Usage = D3D11_USAGE_DEFAULT;
  destination.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> nv12;
  const Nv12TextureCacheKey requested_key{{destination.Width, destination.Height},
                                           destination.Format};
  const bool reuse_cached = ShouldReuseNv12OutputTexture(
      allow_output_texture_reuse, cached_output_key_, requested_key,
      cached_output_device_.Get() == device_.Get());
  if (reuse_cached) {
    nv12 = cached_output_texture_;
  } else {
    last_failure_stage_ = "create_nv12_texture";
    hr = device_->CreateTexture2D(&destination, nullptr, &nv12);
    if (FAILED(hr)) return hr;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.ArraySlice = 0;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  last_failure_stage_ = "create_input_view";
  hr = video_device_->CreateVideoProcessorInputView(bgra_source, enumerator_.Get(), &input_desc,
                                                     &input_view);
  if (FAILED(hr)) return hr;

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;
  ComPtr<ID3D11VideoProcessorOutputView> output_view;
  last_failure_stage_ = "create_output_view";
  hr = video_device_->CreateVideoProcessorOutputView(nv12.Get(), enumerator_.Get(), &output_desc,
                                                      &output_view);
  if (FAILED(hr)) return hr;

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.PastFrames = 0;
  stream.FutureFrames = 0;
  stream.pInputSurface = input_view.Get();
  last_failure_stage_ = "video_processor_blt";
  hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
  if (FAILED(hr)) return hr;
  if (allow_output_texture_reuse) {
    // The asynchronous caller must ensure the previous ProcessInput has
    // completed successfully through ProcessOutput before calling Convert
    // again. VideoPipeline's in-flight encode guard provides that invariant.
    cached_output_texture_ = nv12;
    cached_output_device_ = device_;
    cached_output_key_ = requested_key;
  }
  *nv12_output = std::move(nv12);
  return S_OK;
}

}  // namespace pdb::video
