// Copyright 2026 libwebrtc project authors. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef LIB_WEBRTC_WIN_WMF_H264_DECODER_H_
#define LIB_WEBRTC_WIN_WMF_H264_DECODER_H_

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <memory>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder.h"

namespace webrtc {

// H.264 decoded by the GPU through the Microsoft H.264 decoder MFT with a
// D3D11 device manager (DXVA). Without a GPU decoder it fails to configure, and
// errors it cannot recover from return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE, so
// the software fallback wrapper switches to the builtin decoder (FFmpeg). It
// never decodes on the CPU itself.
//
// Decoded frames come back from the GPU through a staging texture owned by
// this class and are converted to I420.
class WmfH264Decoder : public VideoDecoder {
 public:
  WmfH264Decoder();
  ~WmfH264Decoder() override;

  static std::unique_ptr<WmfH264Decoder> Create();
  // True when the default adapter decodes H.264 (DXVA H264_VLD_NOFGT to NV12).
  static bool IsSupported();

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& input_image, bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;
  const char* ImplementationName() const override;

 private:
  HRESULT InitMediaFoundation();
  HRESULT CreateDevice();
  HRESULT CreateDecoder();
  HRESULT ConfigureInputType();
  HRESULT ConfigureOutputType();
  HRESULT ProcessInput(const uint8_t* data, size_t size, int64_t timestamp_us);
  HRESULT ProcessOutput(int64_t timestamp_us, int64_t ntp_time_ms,
                        uint32_t rtp_timestamp);
  // Copies `texture[subresource]` to the staging texture and converts it.
  HRESULT DeliverTexture(ID3D11Texture2D* texture, UINT subresource,
                         int64_t timestamp_us, int64_t ntp_time_ms,
                         uint32_t rtp_timestamp);
  // Drains pending output; false when the MFT failed.
  bool Drain(int64_t timestamp_us, int64_t ntp_time_ms,
             uint32_t rtp_timestamp);

  DecodedImageCallback* callback_ = nullptr;
  bool initialized_ = false;
  bool mf_started_ = false;
  bool com_initialized_ = false;
  int consecutive_errors_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT reset_token_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  Microsoft::WRL::ComPtr<IMFMediaType> output_type_;

  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
};

}  // namespace webrtc

#endif  // LIB_WEBRTC_WIN_WMF_H264_DECODER_H_
