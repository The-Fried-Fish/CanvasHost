/**
 * @file tests/unit/platform/macos/test_nv12_zero_device.mm
 * @brief Verify full-resolution chroma conversion and retained encoder buffers.
 */
#ifdef __APPLE__
  #include "../../../tests_common.h"
  #include "src/platform/macos/av_img_t.h"
  #include "src/platform/macos/nv12_zero_device.h"
  #include "src/video.h"

extern "C" {
  #include <libavutil/frame.h>
}

/** @brief Convert RGB input to 8/10-bit 4:4:4 without subsampling or recycling retained frames. */
TEST(MacosPixelTransfer, Converts444AndPreservesFrameLifetime) {
  for (const bool ten_bit : {false, true}) {
    for (const bool full_range : {false, true}) {
      platf::nv12_zero_device device;
      int capture_format = 0;
      ASSERT_EQ(device.init(nullptr, ten_bit ? platf::pix_fmt_e::yuv444p16 : platf::pix_fmt_e::yuv444p, [](void *, int, int) {
      },
                            [&](void *, int format) {
                              capture_format = format;
                            }),
                0);
      device.colorspace = {video::colorspace_e::rec709, full_range, ten_bit ? 10u : 8u};
      auto *frame = av_frame_alloc();
      ASSERT_NE(frame, nullptr);
      frame->width = 64;
      frame->height = 64;
      ASSERT_EQ(device.set_frame(frame, nullptr), 0);
      device.apply_colorspace();

      CVPixelBufferRef pixel = nullptr;
      ASSERT_EQ(CVPixelBufferCreate(nullptr, 64, 64, capture_format, nullptr, &pixel), kCVReturnSuccess);
      CVPixelBufferLockBaseAddress(pixel, 0);
      memset(CVPixelBufferGetBaseAddress(pixel), 128, CVPixelBufferGetBytesPerRow(pixel) * 64);
      CVPixelBufferUnlockBaseAddress(pixel, 0);
      CMVideoFormatDescriptionRef format = nullptr;
      ASSERT_EQ(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixel, &format), noErr);
      CMSampleTimingInfo timing = {CMTimeMake(1, 60), kCMTimeZero, kCMTimeInvalid};
      CMSampleBufferRef sample = nullptr;
      ASSERT_EQ(CMSampleBufferCreateReadyWithImageBuffer(nullptr, pixel, format, &timing, &sample), noErr);
      platf::av_img_t image;
      image.sample_buffer = std::make_shared<platf::av_sample_buf_t>(sample);
      image.pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(sample);
      CFRelease(sample);
      CFRelease(format);
      CFRelease(pixel);

      ASSERT_EQ(device.convert(image), 0);
      auto first = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
      ASSERT_NE(first, nullptr);
      EXPECT_EQ(CVPixelBufferGetWidthOfPlane(first, 1), 64);
      EXPECT_EQ(CVPixelBufferGetHeightOfPlane(first, 1), 64);
      const auto expected = ten_bit ?
                              (full_range ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange : kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange) :
                              (full_range ? kCVPixelFormatType_444YpCbCr8BiPlanarFullRange : kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);
      EXPECT_EQ(CVPixelBufferGetPixelFormatType(first), expected);
      AVBufferRef *retained = av_buffer_ref(frame->buf[0]);
      ASSERT_NE(retained, nullptr);
      ASSERT_EQ(device.convert(image), 0);
      EXPECT_NE(reinterpret_cast<CVPixelBufferRef>(frame->data[3]), first);
      EXPECT_EQ(CVPixelBufferGetWidth(first), 64);
      av_buffer_unref(&retained);
    }
  }
}
#endif
