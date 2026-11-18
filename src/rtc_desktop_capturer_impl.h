/*
 * Copyright 2022 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "common_video/include/video_frame_buffer_pool.h"
#include "include/rtc_desktop_capturer.h"
#include "include/rtc_desktop_device.h"
#include "include/rtc_types.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/thread.h"
#ifdef WEBRTC_WIN
#include "rtc_base/win/scoped_com_initializer.h"
#endif
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

namespace libwebrtc {

class RTCDesktopCapturerImpl : public RTCDesktopCapturer,
                               public webrtc::DesktopCapturer::Callback,
                               public webrtc::internal::VideoCapturer {
 public:
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source, bool showCursor = true);
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source,
                         const RTCDesktopCapturerOptions& options);
  ~RTCDesktopCapturerImpl();

  void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) override {
    observer_ = observer;
  }

  void DeRegisterDesktopCapturerObserver() override { observer_ = nullptr; }
  CaptureState Start(uint32_t fps) override;

  CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) override;

  void Stop() override;

  bool IsRunning() override;

  scoped_refptr<MediaSource> source() override { return source_; }

  bool IsCaptureBorderHidden() override { return border_hidden_; }

 protected:
  virtual void OnCaptureResult(
      webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 private:
  void CaptureFrame();
  // Converts a frame from the Windows.Graphics.Capture capturer.
  void OnWgcFrame(const webrtc::DesktopFrame& frame);
  // Resenha: a buffer the encoder is not reading. Reusing one buffer let a
  // new capture overwrite a frame still being encoded.
  webrtc::scoped_refptr<webrtc::I420Buffer> NextBuffer(int width, int height);
  webrtc::DesktopCaptureOptions options_;
  std::unique_ptr<webrtc::DesktopCapturer> capturer_;
  std::unique_ptr<webrtc::Thread> thread_;
  webrtc::VideoFrameBufferPool buffer_pool_{/*zero_initialize=*/false,
                                            /*max_number_of_buffers=*/8};
  CaptureState capture_state_ = CS_STOPPED;
  DesktopType type_;
  webrtc::DesktopCapturer::SourceId source_id_;
  DesktopCapturerObserver* observer_ = nullptr;
  // Resenha: time between captures, and when the next one is due. The wait
  // discounts the time a capture takes, so 60 fps means 60 fps.
  int64_t capture_period_us_ = 1000000;  // 1s
  int64_t next_capture_us_ = 0;
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::SUCCESS;
  webrtc::Thread* signaling_thread_ = nullptr;
  scoped_refptr<MediaSource> source_;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  uint32_t w_ = 0;
  uint32_t h_ = 0;
  bool focus_window_ = true;
  // Whether capturer_ captures with Windows.Graphics.Capture. Set on thread_
  // in the constructor.
  bool use_wgc_ = false;
  // Whether the WGC capture border is off. Set on thread_ in the constructor.
  bool border_hidden_ = false;
#ifdef WEBRTC_WIN
  // WGC needs COM on the thread it runs on. Created and destroyed on thread_.
  std::unique_ptr<webrtc::ScopedCOMInitializer> com_initializer_;
#endif
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
