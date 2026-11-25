// Copyright 2024 libwebrtc project authors. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include "src/win/wmf_h265_factory.h"

#include "api/environment/environment.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "api/video_codecs/sdp_video_format.h"
#include "media/base/media_constants.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "rtc_base/logging.h"
#include "src/win/wmf_h264_decoder.h"
#include "src/win/wmf_h265_decoder.h"
#include "src/win/wmf_video_encoder.h"

namespace webrtc {

namespace {

// H.265 SDP formats to advertise
std::vector<SdpVideoFormat> GetSupportedH265Formats() {
  return {SdpVideoFormat(kH265CodecName,
                         {{"profile-id", "1"},    // Main profile
                          {"tier-flag", "0"},      // Main tier
                          {"level-id", "120"}})};  // Level 4.0
}

// ------- Encoder factory -------

// H.265 comes from the WMF encoder alone. H.264 comes from the GPU's encoder
// when there is one, with the builtin encoder (OpenH264) behind it: if the GPU
// encoder cannot start or fails mid-stream, the fallback wrapper switches over.
class WmfH265VideoEncoderFactory : public VideoEncoderFactory {
 public:
  explicit WmfH265VideoEncoderFactory(
      std::unique_ptr<VideoEncoderFactory> builtin)
      : builtin_(std::move(builtin)),
        h265_supported_(WmfVideoEncoder::IsSupported(kVideoCodecH265)),
        h264_supported_(WmfVideoEncoder::IsSupported(kVideoCodecH264)) {
    RTC_LOG(LS_INFO) << "WMF H.265 encoder is "
                     << (h265_supported_ ? "available." : "NOT available.");
    RTC_LOG(LS_INFO) << "WMF H.264 hardware encoder is "
                     << (h264_supported_ ? "available." : "NOT available.");
  }

  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override {
    if (h265_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH265CodecName)) {
      auto encoder = WmfVideoEncoder::Create(kVideoCodecH265, format);
      if (encoder) {
        return encoder;
      }
      RTC_LOG(LS_WARNING)
          << "Failed to create WMF H.265 encoder, no fallback available.";
      return nullptr;
    }
    if (h264_supported_ && IsH264NonInterleaved(format)) {
      std::unique_ptr<VideoEncoder> software = builtin_->Create(env, format);
      std::unique_ptr<VideoEncoder> hardware =
          WmfVideoEncoder::Create(kVideoCodecH264, format);
      if (software && hardware) {
        return CreateVideoEncoderSoftwareFallbackWrapper(
            env, std::move(software), std::move(hardware),
            /*prefer_temporal_support=*/false);
      }
      if (hardware) return hardware;
      return software;
    }
    return builtin_->Create(env, format);
  }

  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = builtin_->GetSupportedFormats();
    if (h265_supported_) {
      auto h265_formats = GetSupportedH265Formats();
      formats.insert(formats.end(), h265_formats.begin(),
                     h265_formats.end());
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override {
    if (h265_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH265CodecName)) {
      return {.is_supported = true, .is_power_efficient = true};
    }
    CodecSupport support =
        builtin_->QueryCodecSupport(format, scalability_mode);
    if (support.is_supported && h264_supported_ &&
        IsH264NonInterleaved(format)) {
      support.is_power_efficient = true;
    }
    return support;
  }

 private:
  // The GPU encoder writes whole frames that may exceed a packet, so it is
  // only used with packetization-mode=1 (fragmentation allowed). That is the
  // mode LiveKit negotiates; mode 0 stays with OpenH264.
  static bool IsH264NonInterleaved(const SdpVideoFormat& format) {
    if (!absl::EqualsIgnoreCase(format.name, kH264CodecName)) return false;
    auto it = format.parameters.find(kH264FmtpPacketizationMode);
    return it != format.parameters.end() && it->second == "1";
  }

  std::unique_ptr<VideoEncoderFactory> builtin_;
  bool h265_supported_;
  bool h264_supported_;
};

// ------- Decoder factory -------

// H.265 comes from the WMF decoder alone. H.264 comes from the GPU's decoder
// when there is one, with the builtin decoder (FFmpeg) behind it: if the GPU
// fails to configure or gives up decoding, the wrapper switches to software.
class WmfH265VideoDecoderFactory : public VideoDecoderFactory {
 public:
  explicit WmfH265VideoDecoderFactory(
      std::unique_ptr<VideoDecoderFactory> builtin)
      : builtin_(std::move(builtin)),
        h265_supported_(WmfH265Decoder::IsSupported()),
        h264_supported_(WmfH264Decoder::IsSupported()) {
    if (h265_supported_) {
      RTC_LOG(LS_INFO) << "WMF H.265 decoder is available.";
    } else {
      RTC_LOG(LS_WARNING) << "WMF H.265 decoder is NOT available.";
    }
    RTC_LOG(LS_INFO) << "WMF H.264 decoder is "
                     << (h264_supported_ ? "available." : "NOT available.");
  }

  std::unique_ptr<VideoDecoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override {
    if (h265_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH265CodecName)) {
      auto decoder = WmfH265Decoder::Create();
      if (decoder) {
        return decoder;
      }
      RTC_LOG(LS_WARNING)
          << "Failed to create WMF H.265 decoder, no fallback available.";
      return nullptr;
    }
    if (h264_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH264CodecName)) {
      std::unique_ptr<VideoDecoder> software = builtin_->Create(env, format);
      std::unique_ptr<VideoDecoder> hardware = WmfH264Decoder::Create();
      if (software && hardware) {
        return CreateVideoDecoderSoftwareFallbackWrapper(
            env, std::move(software), std::move(hardware));
      }
      if (software) return software;
      return hardware;
    }
    return builtin_->Create(env, format);
  }

  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = builtin_->GetSupportedFormats();
    if (h265_supported_) {
      auto h265_formats = GetSupportedH265Formats();
      formats.insert(formats.end(), h265_formats.begin(),
                     h265_formats.end());
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const SdpVideoFormat& format,
      bool reference_scaling) const override {
    if (h265_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH265CodecName)) {
      return {.is_supported = true, .is_power_efficient = true};
    }
    CodecSupport support =
        builtin_->QueryCodecSupport(format, reference_scaling);
    if (support.is_supported && h264_supported_ &&
        absl::EqualsIgnoreCase(format.name, kH264CodecName)) {
      support.is_power_efficient = true;
    }
    return support;
  }

 private:
  std::unique_ptr<VideoDecoderFactory> builtin_;
  bool h265_supported_;
  bool h264_supported_;
};

}  // namespace

std::unique_ptr<VideoEncoderFactory> CreateWmfH265EncoderFactory(
    std::unique_ptr<VideoEncoderFactory> builtin) {
  return std::make_unique<WmfH265VideoEncoderFactory>(std::move(builtin));
}

std::unique_ptr<VideoDecoderFactory> CreateWmfH265DecoderFactory(
    std::unique_ptr<VideoDecoderFactory> builtin) {
  return std::make_unique<WmfH265VideoDecoderFactory>(std::move(builtin));
}

}  // namespace webrtc
