// Copyright 2024 libwebrtc project authors. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef LIB_WEBRTC_WIN_WMF_VIDEO_ENCODER_H_
#define LIB_WEBRTC_WIN_WMF_VIDEO_ENCODER_H_

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/h265/h265_bitstream_parser.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

// H.264 or H.265 encoder on a Windows Media Foundation transform (the GPU's
// encoder: NVENC, AMF or Quick Sync).
//
// H.265 takes the best MFT available, software included (WebRTC has no other
// H.265 encoder). H.264 takes hardware MFTs only: without one, OpenH264 is the
// better software encoder, and the factory falls back to it.
class WmfVideoEncoder : public VideoEncoder {
 public:
  WmfVideoEncoder(VideoCodecType codec_type, const SdpVideoFormat& format);
  ~WmfVideoEncoder() override;

  // `codec_type` is kVideoCodecH264 or kVideoCodecH265.
  static std::unique_ptr<WmfVideoEncoder> Create(VideoCodecType codec_type,
                                                 const SdpVideoFormat& format);
  static bool IsSupported(VideoCodecType codec_type);

  int InitEncode(const VideoCodec* codec_settings,
                 const Settings& settings) override;
  int Encode(const VideoFrame& input_image,
             const std::vector<VideoFrameType>* frame_types) override;
  int RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;
  int Release() override;

 private:
  bool IsH264() const { return codec_type_ == kVideoCodecH264; }
  const char* CodecName() const { return IsH264() ? "H.264" : "H.265"; }

  HRESULT InitMediaFoundation();
  HRESULT CreateEncoderMft();
  HRESULT ConfigureInputType();
  HRESULT ConfigureOutputType();
  HRESULT SetOutputTypeWithProfile(UINT32 profile);
  HRESULT ConfigureRateControl();
  HRESULT ProcessInput(const VideoFrame& frame);
  HRESULT ProcessOutput();
  // Async (hardware) MFT helpers. Hardware encoders are asynchronous MFTs:
  // they must be unlocked and driven through their event queue instead of the
  // plain ProcessInput()/ProcessOutput() polling loop.
  HRESULT PumpEvent(bool wait);
  HRESULT EncodeAsync(const VideoFrame& frame);
  // Builds the frame to send: when a keyframe comes without its parameter
  // sets (SPS/PPS, plus VPS in H.265), the last ones seen go in front, so a
  // viewer who joins on a requested keyframe can still decode it.
  std::vector<uint8_t> WithParameterSets(const uint8_t* data, size_t size,
                                         bool* is_keyframe);

  // Timing metadata for a frame handed to the MFT, keyed by the sample time
  // so that output samples can be matched back to their input frame.
  struct FrameTiming {
    int64_t timestamp_us = 0;
    int64_t ntp_time_ms = 0;
    uint32_t rtp_timestamp = 0;
  };

  const VideoCodecType codec_type_;
  const SdpVideoFormat format_;
  EncodedImageCallback* callback_ = nullptr;
  VideoCodec codec_settings_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  uint32_t target_bitrate_bps_ = 0;
  uint32_t max_bitrate_bps_ = 0;
  uint32_t frame_rate_ = 30;
  bool initialized_ = false;
  bool mf_started_ = false;
  bool com_initialized_ = false;

  Microsoft::WRL::ComPtr<IMFTransform> encoder_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  Microsoft::WRL::ComPtr<IMFMediaType> input_type_;
  Microsoft::WRL::ComPtr<IMFMediaType> output_type_;

  // True when the activated MFT is an asynchronous (hardware) transform.
  bool is_async_ = false;
  // Number of METransformNeedInput events received but not yet consumed.
  int pending_input_requests_ = 0;
  std::map<LONGLONG, FrameTiming> frame_timing_;
  FrameTiming last_timing_;

  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;

  std::vector<uint8_t> nv12_buffer_;
  // Last parameter-set NAL units (start codes included), in stream order.
  std::vector<uint8_t> parameter_sets_;
  H264BitstreamParser h264_parser_;
  H265BitstreamParser h265_parser_;
};

}  // namespace webrtc

#endif  // LIB_WEBRTC_WIN_WMF_VIDEO_ENCODER_H_
