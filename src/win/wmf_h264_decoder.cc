// Copyright 2026 libwebrtc project authors. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include "src/win/wmf_h264_decoder.h"

#include <d3d10.h>
#include <mftransform.h>

#include <algorithm>
#include <vector>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace webrtc {

namespace {

constexpr char kImplementationName[] = "WMF_H264";

// Errors in a row before giving up on the GPU and asking for the fallback.
constexpr int kMaxConsecutiveErrors = 5;

// Output format changes handled in a single drain before treating it as a
// failure (a decoder that keeps changing format would otherwise loop).
constexpr int kMaxStreamChangesPerDrain = 4;

Microsoft::WRL::ComPtr<ID3D11Device> CreateVideoDevice() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, nullptr);
  if (FAILED(hr)) return nullptr;
  return device;
}

}  // namespace

WmfH264Decoder::WmfH264Decoder() {}

WmfH264Decoder::~WmfH264Decoder() {
  Release();
}

std::unique_ptr<WmfH264Decoder> WmfH264Decoder::Create() {
  if (!IsSupported()) {
    RTC_LOG(LS_WARNING) << "WMF H.264 decoder is not supported on this system.";
    return nullptr;
  }
  return std::make_unique<WmfH264Decoder>();
}

bool WmfH264Decoder::IsSupported() {
  Microsoft::WRL::ComPtr<ID3D11Device> device = CreateVideoDevice();
  if (!device) return false;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
  if (FAILED(device.As(&video_device))) return false;

  const UINT count = video_device->GetVideoDecoderProfileCount();
  for (UINT i = 0; i < count; i++) {
    GUID profile = {};
    if (FAILED(video_device->GetVideoDecoderProfile(i, &profile))) continue;
    if (profile != D3D11_DECODER_PROFILE_H264_VLD_NOFGT) continue;
    BOOL supported = FALSE;
    HRESULT hr = video_device->CheckVideoDecoderFormat(
        &profile, DXGI_FORMAT_NV12, &supported);
    return SUCCEEDED(hr) && supported;
  }
  return false;
}

HRESULT WmfH264Decoder::InitMediaFoundation() {
  HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(com_hr)) {
    com_initialized_ = true;
  } else if (com_hr != RPC_E_CHANGED_MODE) {
    RTC_LOG(LS_ERROR) << "CoInitializeEx failed: " << static_cast<int>(com_hr);
    return com_hr;
  }

  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFStartup failed: " << static_cast<int>(hr);
    return hr;
  }
  mf_started_ = true;
  return S_OK;
}

HRESULT WmfH264Decoder::CreateDevice() {
  device_ = CreateVideoDevice();
  if (!device_) {
    RTC_LOG(LS_ERROR) << "D3D11 device with video support not available.";
    return E_FAIL;
  }
  device_->GetImmediateContext(&context_);

  // The MFT and this class (staging copies) share the immediate context.
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &device_manager_);
  if (FAILED(hr)) return hr;
  return device_manager_->ResetDevice(device_.Get(), reset_token_);
}

HRESULT WmfH264Decoder::CreateDecoder() {
  MFT_REGISTER_TYPE_INFO input_type = {MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activate = nullptr;
  UINT32 count = 0;

  // Synchronous MFTs only: that is the Microsoft decoder, which uses the GPU
  // through the device manager. Vendor async MFTs are left out.
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &input_type, nullptr, &activate, &count);
  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "No H.264 decoder MFT found. hr="
                      << static_cast<int>(hr);
    if (activate) CoTaskMemFree(activate);
    return E_FAIL;
  }

  for (UINT32 i = 0; i < count && !decoder_; i++) {
    Microsoft::WRL::ComPtr<IMFTransform> candidate;
    if (FAILED(activate[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
      continue;
    }
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    UINT32 d3d11_aware = 0;
    if (SUCCEEDED(candidate->GetAttributes(&attributes)) &&
        SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) &&
        d3d11_aware) {
      attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
      decoder_ = candidate;
    } else {
      activate[i]->ShutdownObject();
    }
  }
  for (UINT32 i = 0; i < count; i++) activate[i]->Release();
  CoTaskMemFree(activate);

  if (!decoder_) {
    RTC_LOG(LS_ERROR) << "No D3D11-aware H.264 decoder MFT.";
    return E_FAIL;
  }

  hr = decoder_->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "H.264 decoder refused the D3D11 device: "
                      << static_cast<int>(hr);
    return hr;
  }
  return S_OK;
}

HRESULT WmfH264Decoder::ConfigureInputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;

  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (width_ > 0 && height_ > 0) {
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
  }

  hr = decoder_->SetInputType(input_stream_id_, type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetInputType failed: " << static_cast<int>(hr);
  }
  return hr;
}

HRESULT WmfH264Decoder::ConfigureOutputType() {
  for (DWORD index = 0;; index++) {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT hr =
        decoder_->GetOutputAvailableType(output_stream_id_, index, &type);
    if (FAILED(hr)) break;
    GUID subtype = {};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        subtype != MFVideoFormat_NV12) {
      continue;
    }
    hr = decoder_->SetOutputType(output_stream_id_, type.Get(), 0);
    if (SUCCEEDED(hr)) {
      output_type_ = type;
      UINT32 w = 0, h = 0;
      if (SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) &&
          w > 0 && h > 0) {
        width_ = static_cast<int32_t>(w);
        height_ = static_cast<int32_t>(h);
      }
      return S_OK;
    }
  }
  RTC_LOG(LS_ERROR) << "H.264 decoder offers no NV12 output.";
  return E_FAIL;
}

bool WmfH264Decoder::Configure(const Settings& settings) {
  Release();
  if (settings.codec_type() != kVideoCodecH264) return false;

  if (settings.max_render_resolution().Valid()) {
    width_ = settings.max_render_resolution().Width();
    height_ = settings.max_render_resolution().Height();
  }

  bool ok = SUCCEEDED(InitMediaFoundation()) && SUCCEEDED(CreateDevice()) &&
            SUCCEEDED(CreateDecoder());
  if (ok) {
    DWORD input_count = 0, output_count = 0;
    if (SUCCEEDED(decoder_->GetStreamCount(&input_count, &output_count)) &&
        input_count > 0 && output_count > 0) {
      std::vector<DWORD> input_ids(input_count), output_ids(output_count);
      if (SUCCEEDED(decoder_->GetStreamIDs(input_count, input_ids.data(),
                                           output_count, output_ids.data()))) {
        input_stream_id_ = input_ids[0];
        output_stream_id_ = output_ids[0];
      }
    }
    ok = SUCCEEDED(ConfigureInputType()) && SUCCEEDED(ConfigureOutputType());
  }
  if (!ok) {
    Release();
    return false;
  }

  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  initialized_ = true;
  consecutive_errors_ = 0;
  RTC_LOG(LS_INFO) << "WMF H.264 decoder initialized (GPU).";
  return true;
}

HRESULT WmfH264Decoder::ProcessInput(const uint8_t* data, size_t size,
                                     int64_t timestamp_us) {
  Microsoft::WRL::ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(&sample);
  if (FAILED(hr)) return hr;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
  if (FAILED(hr)) return hr;

  BYTE* buffer_data = nullptr;
  hr = buffer->Lock(&buffer_data, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  memcpy(buffer_data, data, size);
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(size));

  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(timestamp_us * 10);  // 100 ns units

  return decoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
}

HRESULT WmfH264Decoder::DeliverTexture(ID3D11Texture2D* texture,
                                       UINT subresource, int64_t timestamp_us,
                                       int64_t ntp_time_ms,
                                       uint32_t rtp_timestamp) {
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_NV12) {
    RTC_LOG(LS_ERROR) << "H.264 decoder texture is not NV12: "
                      << static_cast<int>(desc.Format);
    return E_FAIL;
  }

  D3D11_TEXTURE2D_DESC staging_desc = {};
  if (staging_) staging_->GetDesc(&staging_desc);
  if (!staging_ || staging_desc.Width != desc.Width ||
      staging_desc.Height != desc.Height) {
    staging_.Reset();
    staging_desc = {};
    staging_desc.Width = desc.Width;
    staging_desc.Height = desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_NV12;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging_);
    if (FAILED(hr)) return hr;
  }

  context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture,
                                  subresource, nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) return hr;

  // The surface is padded (1088 rows for 1080p); show only the display area.
  int width = static_cast<int>(desc.Width);
  int height = static_cast<int>(desc.Height);
  MFVideoArea area = {};
  if (output_type_ &&
      SUCCEEDED(output_type_->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                      reinterpret_cast<UINT8*>(&area),
                                      static_cast<UINT32>(sizeof(area)), nullptr)) &&
      area.Area.cx > 0 && area.Area.cy > 0) {
    width = std::min<int>(width, area.Area.cx);
    height = std::min<int>(height, area.Area.cy);
  } else if (width_ > 0 && height_ > 0) {
    width = std::min<int>(width, width_);
    height = std::min<int>(height, height_);
  }

  const uint8_t* y = static_cast<const uint8_t*>(mapped.pData);
  const uint8_t* uv = y + static_cast<size_t>(mapped.RowPitch) * desc.Height;
  scoped_refptr<I420Buffer> i420 = I420Buffer::Create(width, height);
  const int pitch = static_cast<int>(mapped.RowPitch);
  libyuv::NV12ToI420(y, pitch, uv, pitch,
                     i420->MutableDataY(), i420->StrideY(),
                     i420->MutableDataU(), i420->StrideU(),
                     i420->MutableDataV(), i420->StrideV(), width, height);
  context_->Unmap(staging_.Get(), 0);

  if (callback_) {
    VideoFrame frame = VideoFrame::Builder()
                           .set_video_frame_buffer(i420)
                           .set_timestamp_rtp(rtp_timestamp)
                           .set_ntp_time_ms(ntp_time_ms)
                           .set_timestamp_us(timestamp_us)
                           .build();
    callback_->Decoded(frame);
  }
  return S_OK;
}

HRESULT WmfH264Decoder::ProcessOutput(int64_t timestamp_us,
                                      int64_t ntp_time_ms,
                                      uint32_t rtp_timestamp) {
  MFT_OUTPUT_STREAM_INFO stream_info = {};
  HRESULT hr = decoder_->GetOutputStreamInfo(output_stream_id_, &stream_info);
  if (FAILED(hr)) return hr;
  // With the device manager the MFT hands out its own D3D11 samples; anything
  // else means it is not decoding on the GPU.
  if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
    RTC_LOG(LS_ERROR) << "H.264 decoder does not provide GPU samples.";
    return E_FAIL;
  }

  MFT_OUTPUT_DATA_BUFFER output = {};
  output.dwStreamID = output_stream_id_;
  DWORD status = 0;
  hr = decoder_->ProcessOutput(0, 1, &output, &status);
  Microsoft::WRL::ComPtr<IMFSample> sample;
  sample.Attach(output.pSample);
  if (output.pEvents) output.pEvents->Release();

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    HRESULT config_hr = ConfigureOutputType();
    return FAILED(config_hr) ? config_hr : hr;
  }
  if (FAILED(hr)) return hr;
  if (!sample) return E_FAIL;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  hr = sample->GetBufferByIndex(0, &buffer);
  if (FAILED(hr)) return hr;
  Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgi_buffer;
  if (FAILED(buffer.As(&dxgi_buffer))) {
    RTC_LOG(LS_ERROR) << "H.264 decoder output is not a GPU texture.";
    return E_FAIL;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  hr = dxgi_buffer->GetResource(IID_PPV_ARGS(&texture));
  if (FAILED(hr)) return hr;
  UINT subresource = 0;
  hr = dxgi_buffer->GetSubresourceIndex(&subresource);
  if (FAILED(hr)) return hr;

  return DeliverTexture(texture.Get(), subresource, timestamp_us, ntp_time_ms,
                        rtp_timestamp);
}

bool WmfH264Decoder::Drain(int64_t timestamp_us, int64_t ntp_time_ms,
                           uint32_t rtp_timestamp) {
  int stream_changes = 0;
  while (true) {
    HRESULT hr = ProcessOutput(timestamp_us, ntp_time_ms, rtp_timestamp);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (++stream_changes > kMaxStreamChangesPerDrain) return false;
      continue;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "H.264 decoder ProcessOutput failed: "
                        << static_cast<int>(hr);
      return false;
    }
  }
}

int32_t WmfH264Decoder::Decode(const EncodedImage& input_image,
                               bool /*missing_frames*/,
                               int64_t /*render_time_ms*/) {
  if (!initialized_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (input_image.data() == nullptr || input_image.size() == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  const int64_t timestamp_us =
      static_cast<int64_t>(input_image.RtpTimestamp()) * 1000 / 90;
  const int64_t ntp_time_ms = input_image.ntp_time_ms_;
  const uint32_t rtp_timestamp = input_image.RtpTimestamp();

  HRESULT hr =
      ProcessInput(input_image.data(), input_image.size(), timestamp_us);
  if (hr == MF_E_NOTACCEPTING &&
      Drain(timestamp_us, ntp_time_ms, rtp_timestamp)) {
    hr = ProcessInput(input_image.data(), input_image.size(), timestamp_us);
  }
  bool ok = SUCCEEDED(hr) && Drain(timestamp_us, ntp_time_ms, rtp_timestamp);
  if (ok) {
    consecutive_errors_ = 0;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  RTC_LOG(LS_WARNING) << "WMF H.264 decode failed: " << static_cast<int>(hr);
  if (++consecutive_errors_ >= kMaxConsecutiveErrors) {
    RTC_LOG(LS_WARNING) << "WMF H.264 decoder gave up; switching to software.";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  return WEBRTC_VIDEO_CODEC_ERROR;  // WebRTC asks for a key frame.
}

int32_t WmfH264Decoder::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WmfH264Decoder::Release() {
  if (decoder_) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    decoder_.Reset();
  }
  output_type_.Reset();
  staging_.Reset();
  device_manager_.Reset();
  context_.Reset();
  device_.Reset();

  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }
  initialized_ = false;
  width_ = 0;
  height_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

const char* WmfH264Decoder::ImplementationName() const {
  return kImplementationName;
}

VideoDecoder::DecoderInfo WmfH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = kImplementationName;
  info.is_hardware_accelerated = true;
  return info;
}

}  // namespace webrtc
