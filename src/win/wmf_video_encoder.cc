// Copyright 2024 libwebrtc project authors. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include "src/win/wmf_video_encoder.h"

#include <codecapi.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>

#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/codecs/interface/common_constants.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace webrtc {
namespace {

// Media Foundation HRESULTs are only recognizable in hex.
std::string HrStr(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return std::string(buf);
}

// Upper bound on events serviced in a single Encode() call, so a misbehaving
// MFT can never spin the encoder thread forever.
constexpr int kMaxEventsPerEncode = 256;
// Upper bound on frames in flight inside the MFT that we keep timing for.
constexpr size_t kMaxPendingFrameTimings = 64;

GUID SubtypeOf(VideoCodecType codec_type) {
  return codec_type == kVideoCodecH264 ? MFVideoFormat_H264
                                       : MFVideoFormat_HEVC;
}

// H.265 keeps the flags it shipped with (software MFTs included). H.264 asks
// for hardware MFTs only.
UINT32 EnumFlagsOf(VideoCodecType codec_type) {
  if (codec_type == kVideoCodecH264) {
    return MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
  }
  return MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
}

// One Annex B NAL unit: where its start code begins, where its header byte
// is, and where it ends (the next start code or the end of the buffer).
struct Nalu {
  size_t start = 0;
  size_t header = 0;
  size_t end = 0;
};

std::vector<Nalu> SplitNalus(const uint8_t* data, size_t size) {
  std::vector<Nalu> nalus;
  size_t i = 0;
  while (i + 3 <= size) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      // A 4-byte start code is a zero byte followed by the 3-byte one.
      size_t start = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
      if (!nalus.empty()) nalus.back().end = start;
      nalus.push_back({start, i + 3, size});
      i += 3;
    } else {
      ++i;
    }
  }
  // Drop units without a header byte (a stray start code at the very end).
  nalus.erase(std::remove_if(nalus.begin(), nalus.end(),
                             [](const Nalu& n) { return n.header >= n.end; }),
              nalus.end());
  return nalus;
}

uint8_t NaluType(VideoCodecType codec_type, uint8_t header) {
  return codec_type == kVideoCodecH264 ? (header & 0x1F)
                                       : ((header >> 1) & 0x3F);
}

bool IsParameterSet(VideoCodecType codec_type, uint8_t type) {
  // H.264: SPS(7), PPS(8). H.265: VPS(32), SPS(33), PPS(34).
  return codec_type == kVideoCodecH264 ? (type == 7 || type == 8)
                                       : (type >= 32 && type <= 34);
}

bool IsKeyframeSlice(VideoCodecType codec_type, uint8_t type) {
  // H.264: IDR slice(5). H.265: IRAP VCL units, BLA_W_LP(16) through
  // RSV_IRAP_VCL23(23).
  return codec_type == kVideoCodecH264 ? type == 5
                                       : (type >= 16 && type <= 23);
}

// The H.264 profile the session negotiated, as a Media Foundation profile.
// Constrained Baseline first: it is what Baseline means to WebRTC peers.
std::vector<UINT32> H264ProfilesFor(const SdpVideoFormat& format) {
  std::optional<H264ProfileLevelId> id =
      ParseSdpForH264ProfileLevelId(format.parameters);
  H264Profile profile =
      id ? id->profile : H264Profile::kProfileConstrainedBaseline;
  switch (profile) {
    case H264Profile::kProfileMain:
      return {eAVEncH264VProfile_Main};
    case H264Profile::kProfileConstrainedHigh:
    case H264Profile::kProfileHigh:
      return {eAVEncH264VProfile_High};
    default:
      return {eAVEncH264VProfile_ConstrainedBase, eAVEncH264VProfile_Base};
  }
}

}  // namespace

WmfVideoEncoder::WmfVideoEncoder(VideoCodecType codec_type,
                                 const SdpVideoFormat& format)
    : codec_type_(codec_type), format_(format) {
  RTC_DCHECK(codec_type == kVideoCodecH264 || codec_type == kVideoCodecH265);
}

WmfVideoEncoder::~WmfVideoEncoder() {
  Release();
}

std::unique_ptr<WmfVideoEncoder> WmfVideoEncoder::Create(
    VideoCodecType codec_type,
    const SdpVideoFormat& format) {
  if (!IsSupported(codec_type)) {
    RTC_LOG(LS_ERROR) << "WMF " << CodecTypeToPayloadString(codec_type)
                      << " encoder is not supported on this system.";
    return nullptr;
  }
  return std::make_unique<WmfVideoEncoder>(codec_type, format);
}

bool WmfVideoEncoder::IsSupported(VideoCodecType codec_type) {
  // Ensure COM is initialized on this thread (MF requires it)
  HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool should_uninit_com = SUCCEEDED(com_hr);

  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) {
    if (should_uninit_com) CoUninitialize();
    return false;
  }

  MFT_REGISTER_TYPE_INFO output_type = {MFMediaType_Video,
                                        SubtypeOf(codec_type)};
  IMFActivate** activate = nullptr;
  UINT32 count = 0;

  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, EnumFlagsOf(codec_type), nullptr,
                 &output_type, &activate, &count);

  bool supported = SUCCEEDED(hr) && count > 0;

  if (activate) {
    for (UINT32 i = 0; i < count; i++) {
      activate[i]->Release();
    }
    CoTaskMemFree(activate);
  }

  MFShutdown();
  if (should_uninit_com) CoUninitialize();
  return supported;
}

HRESULT WmfVideoEncoder::InitMediaFoundation() {
  // Ensure COM is initialized on the calling thread
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
    if (com_initialized_) {
      CoUninitialize();
      com_initialized_ = false;
    }
    return hr;
  }
  mf_started_ = true;
  return S_OK;
}

HRESULT WmfVideoEncoder::CreateEncoderMft() {
  // Enumerate and activate encoders (hardware first)
  MFT_REGISTER_TYPE_INFO output_type = {MFMediaType_Video,
                                        SubtypeOf(codec_type_)};
  IMFActivate** activate = nullptr;
  UINT32 count = 0;

  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, EnumFlagsOf(codec_type_),
                         nullptr, &output_type, &activate, &count);

  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "No " << CodecName()
                      << " encoder MFT found. hr=" << HrStr(hr);
    if (activate) CoTaskMemFree(activate);
    return E_FAIL;
  }

  // Log available encoders
  for (UINT32 i = 0; i < count; i++) {
    LPWSTR friendly_name = nullptr;
    UINT32 name_len = 0;
    activate[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                    &friendly_name, &name_len);
    if (friendly_name) {
      RTC_LOG(LS_INFO) << "Found " << CodecName() << " encoder: "
                       << webrtc::ToUtf8(friendly_name, name_len);
      CoTaskMemFree(friendly_name);
    }
  }

  // Activate the first (highest priority) encoder
  hr = activate[0]->ActivateObject(IID_PPV_ARGS(&encoder_));

  for (UINT32 i = 0; i < count; i++) {
    activate[i]->Release();
  }
  CoTaskMemFree(activate);

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to activate " << CodecName()
                      << " encoder MFT: " << HrStr(hr);
    return hr;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  hr = encoder_->GetAttributes(&attributes);
  if (SUCCEEDED(hr) && attributes) {
    // Hardware encoders are asynchronous MFTs and stay locked until the
    // client opts in. Every call on a locked MFT (SetInputType included)
    // fails with MF_E_TRANSFORM_ASYNC_LOCKED (0xC00D6D77).
    UINT32 is_async = 0;
    if (FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async))) {
      is_async = 0;
    }
    if (is_async) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "Failed to unlock async " << CodecName()
                          << " encoder MFT: " << HrStr(hr);
        return hr;
      }
      is_async_ = true;
    }
    // Set low-latency mode for real-time encoding.
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
  }

  if (IsH264() && !is_async_) {
    // Only hardware MFTs were enumerated, and those are asynchronous.
    RTC_LOG(LS_ERROR) << "H.264 encoder MFT is not a hardware transform.";
    return E_FAIL;
  }

  if (is_async_) {
    hr = encoder_.As(&event_generator_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "Async " << CodecName()
                        << " MFT has no event generator: " << HrStr(hr);
      return hr;
    }
    // Tell the hardware MFT that no DXGI device manager will be supplied, so
    // it configures itself for system-memory input.
    encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
  }

  if (IsH264()) {
    // WebRTC sends frames in capture order: no B-frames. Most encoders only
    // take this before the media types are set.
    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(encoder_.As(&codec_api))) {
      VARIANT var;
      VariantInit(&var);
      var.vt = VT_UI4;
      var.ulVal = 0;
      codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);
      VariantInit(&var);
      var.vt = VT_BOOL;
      var.boolVal = VARIANT_TRUE;
      codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    }
  }

  RTC_LOG(LS_INFO) << CodecName() << " encoder MFT activated successfully ("
                   << (is_async_ ? "async/hardware" : "sync") << ").";
  return S_OK;
}

HRESULT WmfVideoEncoder::ConfigureRateControl() {
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  HRESULT hr = encoder_.As(&codec_api);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "ICodecAPI not available, skipping rate control setup.";
    return S_OK;  // Non-fatal: MFT will use its default
  }

  // Peak-constrained VBR with the peak at the target and a one second buffer:
  // the encoder may spend less on easy frames but never goes over. Plain CBR
  // overshot (10-13 Mbps for 8 Mbps on AMD). Must be set before the media
  // types, or the MFT keeps its default mode.
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
  hr = codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Peak-constrained VBR not supported ("
                        << static_cast<int>(hr) << "), using CBR.";
    var.ulVal = eAVEncCommonRateControlMode_CBR;
    hr = codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "Failed to set CBR rate control: "
                          << static_cast<int>(hr);
    }
  }
  if (target_bitrate_bps_ > 0) SetBitrateValues(codec_api.Get(), target_bitrate_bps_);

  return S_OK;
}

bool WmfVideoEncoder::SetBitrateValues(ICodecAPI* codec_api, uint32_t bps) {
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = bps;
  const bool ok =
      SUCCEEDED(codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var));
  codec_api->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);
  codec_api->SetValue(&CODECAPI_AVEncCommonBufferSize, &var);  // 1 s, in bits
  return ok;
}

HRESULT WmfVideoEncoder::SetOutputTypeWithProfile(UINT32 profile) {
  HRESULT hr = MFCreateMediaType(&output_type_);
  if (FAILED(hr)) return hr;

  output_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type_->SetGUID(MF_MT_SUBTYPE, SubtypeOf(codec_type_));
  MFSetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(output_type_.Get(), MF_MT_FRAME_RATE, frame_rate_, 1);
  output_type_->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate_bps_);
  output_type_->SetUINT32(MF_MT_INTERLACE_MODE,
                          MFVideoInterlace_Progressive);
  MFSetAttributeRatio(output_type_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output_type_->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
  if (!IsH264()) {
    // H.265 Main, level 4.0. H.264 leaves the level to the encoder, which
    // picks it from the frame size and rate.
    output_type_->SetUINT32(MF_MT_MPEG2_LEVEL, 120);
  }

  return encoder_->SetOutputType(output_stream_id_, output_type_.Get(), 0);
}

HRESULT WmfVideoEncoder::ConfigureOutputType() {
  std::vector<UINT32> profiles =
      IsH264() ? H264ProfilesFor(format_) : std::vector<UINT32>{1};
  HRESULT hr = E_FAIL;
  for (UINT32 profile : profiles) {
    hr = SetOutputTypeWithProfile(profile);
    if (SUCCEEDED(hr)) {
      RTC_LOG(LS_INFO) << CodecName() << " output type set, profile "
                       << profile;
      return S_OK;
    }
    RTC_LOG(LS_WARNING) << "SetOutputType with profile " << profile
                        << " failed: " << HrStr(hr);
  }
  RTC_LOG(LS_ERROR) << "SetOutputType failed: " << HrStr(hr);
  return hr;
}

HRESULT WmfVideoEncoder::ConfigureInputType() {
  HRESULT hr = MFCreateMediaType(&input_type_);
  if (FAILED(hr)) return hr;

  input_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(input_type_.Get(), MF_MT_FRAME_RATE, frame_rate_, 1);
  input_type_->SetUINT32(MF_MT_INTERLACE_MODE,
                         MFVideoInterlace_Progressive);
  MFSetAttributeRatio(input_type_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  hr = encoder_->SetInputType(input_stream_id_, input_type_.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetInputType failed: " << HrStr(hr);
    return hr;
  }

  return S_OK;
}

int WmfVideoEncoder::InitEncode(const VideoCodec* codec_settings,
                                const Settings& settings) {
  if (!codec_settings) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  Release();

  codec_settings_ = *codec_settings;
  width_ = codec_settings->width;
  height_ = codec_settings->height;
  target_bitrate_bps_ = codec_settings->startBitrate * 1000;
  max_bitrate_bps_ = codec_settings->maxBitrate * 1000;
  frame_rate_ = codec_settings->maxFramerate;
  if (frame_rate_ == 0) frame_rate_ = 30;

  HRESULT hr = InitMediaFoundation();
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  hr = CreateEncoderMft();
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Get stream IDs
  DWORD input_count = 0, output_count = 0;
  hr = encoder_->GetStreamCount(&input_count, &output_count);
  if (SUCCEEDED(hr) && input_count > 0 && output_count > 0) {
    // Try getting explicit IDs
    std::vector<DWORD> input_ids(input_count), output_ids(output_count);
    hr = encoder_->GetStreamIDs(input_count, input_ids.data(), output_count,
                                output_ids.data());
    if (SUCCEEDED(hr)) {
      input_stream_id_ = input_ids[0];
      output_stream_id_ = output_ids[0];
    } else {
      // If E_NOTIMPL, stream IDs are sequential starting from 0
      input_stream_id_ = 0;
      output_stream_id_ = 0;
    }
  }

  // Rate control mode only takes effect before the media types are set.
  hr = ConfigureRateControl();
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Must set output type before input type for most MF encoders
  hr = ConfigureOutputType();
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  hr = ConfigureInputType();
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  applied_bitrate_bps_ = 0;
  ApplyBitrate(/*force=*/true);

  pending_input_requests_ = 0;
  frame_timing_.clear();
  last_timing_ = FrameTiming();
  parameter_sets_.clear();

  // Start processing
  hr = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to start encoder streaming: " << HrStr(hr);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  initialized_ = true;
  RTC_LOG(LS_INFO) << "WMF " << CodecName() << " encoder initialized: "
                   << width_ << "x" << height_ << " @ " << frame_rate_
                   << "fps, " << target_bitrate_bps_ / 1000 << " kbps";

  return WEBRTC_VIDEO_CODEC_OK;
}

HRESULT WmfVideoEncoder::ProcessInput(const VideoFrame& frame) {
  webrtc::scoped_refptr<I420BufferInterface> i420_buffer =
      frame.video_frame_buffer()->ToI420();

  const int nv12_size = width_ * height_ * 3 / 2;

  Microsoft::WRL::ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(&sample);
  if (FAILED(hr)) return hr;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  hr = MFCreateMemoryBuffer(nv12_size, &buffer);
  if (FAILED(hr)) return hr;

  BYTE* buffer_data = nullptr;
  hr = buffer->Lock(&buffer_data, nullptr, nullptr);
  if (FAILED(hr)) return hr;

  // Converts straight into the MFT's buffer (no intermediate copy).
  libyuv::I420ToNV12(i420_buffer->DataY(), i420_buffer->StrideY(),
                     i420_buffer->DataU(), i420_buffer->StrideU(),
                     i420_buffer->DataV(), i420_buffer->StrideV(),
                     buffer_data, width_, buffer_data + width_ * height_,
                     width_, width_, height_);

  hr = buffer->Unlock();
  if (FAILED(hr)) return hr;

  hr = buffer->SetCurrentLength(nv12_size);
  if (FAILED(hr)) return hr;

  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) return hr;

  // Set sample time (100-nanosecond units)
  LONGLONG sample_time = frame.timestamp_us() * 10;
  sample->SetSampleTime(sample_time);
  sample->SetSampleDuration(static_cast<LONGLONG>(10000000.0 / frame_rate_));

  hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
  if (FAILED(hr)) return hr;

  // Remember the frame timing so the matching output sample can carry the
  // upstream RTP timestamp even while the MFT keeps frames in flight.
  FrameTiming timing;
  timing.timestamp_us = frame.timestamp_us();
  timing.ntp_time_ms = frame.ntp_time_ms();
  timing.rtp_timestamp = frame.rtp_timestamp();
  frame_timing_[sample_time] = timing;
  last_timing_ = timing;
  while (frame_timing_.size() > kMaxPendingFrameTimings) {
    frame_timing_.erase(frame_timing_.begin());
  }
  return S_OK;
}

std::vector<uint8_t> WmfVideoEncoder::WithParameterSets(const uint8_t* data,
                                                        size_t size,
                                                        bool* is_keyframe) {
  *is_keyframe = false;
  std::vector<uint8_t> sets;
  for (const Nalu& n : SplitNalus(data, size)) {
    uint8_t type = NaluType(codec_type_, data[n.header]);
    if (IsKeyframeSlice(codec_type_, type)) *is_keyframe = true;
    if (IsParameterSet(codec_type_, type)) {
      sets.insert(sets.end(), data + n.start, data + n.end);
    }
  }
  if (!sets.empty()) {
    parameter_sets_ = std::move(sets);
    return std::vector<uint8_t>(data, data + size);
  }
  std::vector<uint8_t> out;
  if (*is_keyframe) {
    out.reserve(parameter_sets_.size() + size);
    out.insert(out.end(), parameter_sets_.begin(), parameter_sets_.end());
  }
  out.insert(out.end(), data, data + size);
  return out;
}

HRESULT WmfVideoEncoder::ProcessOutput() {
  MFT_OUTPUT_DATA_BUFFER output_data = {};
  output_data.dwStreamID = output_stream_id_;

  // Check if we need to provide the output sample
  MFT_OUTPUT_STREAM_INFO stream_info = {};
  HRESULT hr = encoder_->GetOutputStreamInfo(output_stream_id_, &stream_info);
  if (FAILED(hr)) return hr;

  bool provides_samples =
      (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);

  Microsoft::WRL::ComPtr<IMFSample> output_sample;
  if (!provides_samples) {
    hr = MFCreateSample(&output_sample);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> output_buffer;
    DWORD buffer_size =
        std::max(stream_info.cbSize, static_cast<DWORD>(width_ * height_ * 2));
    hr = MFCreateMemoryBuffer(buffer_size, &output_buffer);
    if (FAILED(hr)) return hr;

    hr = output_sample->AddBuffer(output_buffer.Get());
    if (FAILED(hr)) return hr;

    output_data.pSample = output_sample.Get();
  }

  DWORD status = 0;
  hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return hr;  // Not an error, just need more input
  }

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "ProcessOutput failed: " << HrStr(hr);
    if (output_data.pEvents) output_data.pEvents->Release();
    return hr;
  }

  IMFSample* result_sample =
      provides_samples ? output_data.pSample : output_sample.Get();
  if (!result_sample) {
    if (output_data.pEvents) output_data.pEvents->Release();
    return E_FAIL;
  }

  // Extract encoded data
  Microsoft::WRL::ComPtr<IMFMediaBuffer> result_buffer;
  hr = result_sample->ConvertToContiguousBuffer(&result_buffer);
  if (FAILED(hr)) {
    if (output_data.pEvents) output_data.pEvents->Release();
    return hr;
  }

  BYTE* data = nullptr;
  DWORD data_length = 0;
  hr = result_buffer->Lock(&data, nullptr, &data_length);
  if (FAILED(hr)) {
    if (output_data.pEvents) output_data.pEvents->Release();
    return hr;
  }

  // Match the output sample back to the frame it was produced from; the MFT
  // may hold several frames in flight, so the newest input is not necessarily
  // the frame coming out here.
  FrameTiming timing = last_timing_;
  LONGLONG out_sample_time = 0;
  if (SUCCEEDED(result_sample->GetSampleTime(&out_sample_time))) {
    auto it = frame_timing_.find(out_sample_time);
    if (it != frame_timing_.end()) {
      timing = it->second;
      // Anything older was dropped by the encoder and will never come out.
      frame_timing_.erase(frame_timing_.begin(), std::next(it));
    }
  }

  if (data_length > 0 && callback_) {
    bool is_keyframe = false;
    std::vector<uint8_t> frame =
        WithParameterSets(data, data_length, &is_keyframe);

    EncodedImage encoded_image;
    encoded_image.SetEncodedData(
        EncodedImageBuffer::Create(frame.data(), frame.size()));
    encoded_image._encodedWidth = width_;
    encoded_image._encodedHeight = height_;
    // Preserve the RTP timestamp from the upstream WebRTC frame
    // to avoid drift between encoder and RTP sender.
    encoded_image.SetRtpTimestamp(timing.rtp_timestamp);
    encoded_image.capture_time_ms_ = timing.timestamp_us / 1000;
    encoded_image.ntp_time_ms_ = timing.ntp_time_ms;
    encoded_image._frameType =
        is_keyframe ? VideoFrameType::kVideoFrameKey
                    : VideoFrameType::kVideoFrameDelta;
    encoded_image.SetSpatialIndex(0);

    // Parse bitstream for QP
    std::span<const uint8_t> bitstream(frame.data(), frame.size());
    std::optional<int> qp;
    if (IsH264()) {
      h264_parser_.ParseBitstream(bitstream);
      qp = h264_parser_.GetLastSliceQp();
    } else {
      h265_parser_.ParseBitstream(bitstream);
      qp = h265_parser_.GetLastSliceQp();
    }
    if (qp.has_value()) {
      encoded_image.qp_ = *qp;
    }

    CodecSpecificInfo codec_info;
    codec_info.codecType = codec_type_;
    if (IsH264()) {
      // The factory only hands out this encoder for packetization-mode=1.
      codec_info.codecSpecific.H264.packetization_mode =
          H264PacketizationMode::NonInterleaved;
      codec_info.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
      codec_info.codecSpecific.H264.base_layer_sync = false;
      codec_info.codecSpecific.H264.idr_frame = is_keyframe;
    }

    bitrate_adjuster_.Update(encoded_image.size());
    ApplyBitrate(/*force=*/false);
    callback_->OnEncodedImage(encoded_image, &codec_info);
  }

  result_buffer->Unlock();

  if (provides_samples && output_data.pSample) {
    output_data.pSample->Release();
  }
  if (output_data.pEvents) {
    output_data.pEvents->Release();
  }

  return S_OK;
}

HRESULT WmfVideoEncoder::PumpEvent(bool wait) {
  Microsoft::WRL::ComPtr<IMFMediaEvent> event;
  HRESULT hr =
      event_generator_->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
  if (FAILED(hr)) return hr;  // Includes MF_E_NO_EVENTS_AVAILABLE.

  MediaEventType type = MEUnknown;
  hr = event->GetType(&type);
  if (FAILED(hr)) return hr;

  switch (type) {
    case METransformNeedInput:
      ++pending_input_requests_;
      return S_OK;
    case METransformHaveOutput:
      hr = ProcessOutput();
      // The MFT is allowed to retract an output request; not an error.
      return hr == MF_E_TRANSFORM_NEED_MORE_INPUT ? S_OK : hr;
    case MEError: {
      HRESULT status = S_OK;
      event->GetStatus(&status);
      RTC_LOG(LS_ERROR) << CodecName()
                        << " MFT reported MEError: " << HrStr(status);
      return FAILED(status) ? status : E_FAIL;
    }
    default:
      // METransformDrainComplete, METransformMarker, ... nothing to do.
      return S_OK;
  }
}

HRESULT WmfVideoEncoder::EncodeAsync(const VideoFrame& frame) {
  int serviced = 0;

  // Wait for the MFT to ask for input, servicing output it produces meanwhile.
  while (pending_input_requests_ == 0) {
    if (++serviced > kMaxEventsPerEncode) {
      RTC_LOG(LS_ERROR) << CodecName()
                        << " MFT never requested input; dropping frame.";
      return MF_E_NOTACCEPTING;
    }
    HRESULT hr = PumpEvent(/*wait=*/true);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "Waiting for " << CodecName()
                        << " MFT input request failed: " << HrStr(hr);
      return hr;
    }
  }

  --pending_input_requests_;
  HRESULT hr = ProcessInput(frame);
  if (FAILED(hr)) return hr;

  // Collect everything already queued, without blocking.
  while (++serviced <= kMaxEventsPerEncode) {
    hr = PumpEvent(/*wait=*/false);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) break;
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

int WmfVideoEncoder::Encode(const VideoFrame& input_image,
                            const std::vector<VideoFrameType>* frame_types) {
  if (!initialized_ || !callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  // Check for keyframe request
  bool force_key_frame = false;
  if (frame_types) {
    for (const auto& frame_type : *frame_types) {
      if (frame_type == VideoFrameType::kVideoFrameKey) {
        force_key_frame = true;
        break;
      }
    }
  }

  if (force_key_frame) {
    // Request IDR from MFT via CODECAPI
    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    HRESULT hr = encoder_.As(&codec_api);
    if (SUCCEEDED(hr)) {
      VARIANT var;
      VariantInit(&var);
      var.vt = VT_UI4;
      var.ulVal = 1;
      codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
    }
  }

  if (is_async_) {
    HRESULT async_hr = EncodeAsync(input_image);
    if (FAILED(async_hr)) {
      RTC_LOG(LS_ERROR) << "Async " << CodecName()
                        << " encode failed: " << HrStr(async_hr);
      // H.264 has OpenH264 behind it (the factory's fallback wrapper); H.265
      // has nothing to fall back to.
      return IsH264() ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                      : WEBRTC_VIDEO_CODEC_ERROR;
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  // Process input
  HRESULT hr = ProcessInput(input_image);
  if (hr == MF_E_NOTACCEPTING) {
    // MFT input buffer full — drain output first, then retry
    while (true) {
      HRESULT out_hr = ProcessOutput();
      if (out_hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(out_hr)) break;
    }
    hr = ProcessInput(input_image);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "ProcessInput failed: " << HrStr(hr);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Try to get output
  while (true) {
    hr = ProcessOutput();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;  // Need more input before getting output
    }
    if (FAILED(hr)) {
      break;
    }
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int WmfVideoEncoder::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

void WmfVideoEncoder::SetRates(const RateControlParameters& parameters) {
  if (!initialized_ || !encoder_) return;

  target_bitrate_bps_ = parameters.bitrate.get_sum_bps();
  frame_rate_ = static_cast<uint32_t>(parameters.framerate_fps);
  if (frame_rate_ == 0) frame_rate_ = 30;

  bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);
  ApplyBitrate(/*force=*/true);
}

void WmfVideoEncoder::ApplyBitrate(bool force) {
  const uint32_t bps = bitrate_adjuster_.GetAdjustedBitrateBps();
  if (bps == 0) return;
  // Each change resets the MFT rate control a little; skip tiny corrections.
  if (!force && applied_bitrate_bps_ != 0) {
    const uint32_t diff = bps > applied_bitrate_bps_ ? bps - applied_bitrate_bps_
                                                       : applied_bitrate_bps_ - bps;
    if (diff * 20 < applied_bitrate_bps_) return;  // under 5%
  }
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  if (FAILED(encoder_.As(&codec_api))) return;
  if (SetBitrateValues(codec_api.Get(), bps)) applied_bitrate_bps_ = bps;
}

VideoEncoder::EncoderInfo WmfVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = IsH264() ? "WMF_H264" : "WMF_H265";
  info.is_hardware_accelerated = true;
  // The MFT runs its own CBR rate control. Without this flag WebRTC's frame
  // dropper also kicks in and, at 1080p60, threw away every other frame
  // (capture at 60 fps, 30 fps out, pinned at the target bitrate).
  info.has_trusted_rate_controller = true;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.requested_resolution_alignment = 2;  // NV12 requires even dimensions
  return info;
}

int WmfVideoEncoder::Release() {
  if (encoder_) {
    // Flush before ending the stream so no trailing output is handed to a
    // callback that may already be gone.
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    encoder_.Reset();
  }

  event_generator_.Reset();
  input_type_.Reset();
  output_type_.Reset();

  is_async_ = false;
  pending_input_requests_ = 0;
  frame_timing_.clear();
  last_timing_ = FrameTiming();
  parameter_sets_.clear();

  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }

  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }

  initialized_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace webrtc
