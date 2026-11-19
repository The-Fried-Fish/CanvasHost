/**
 * @file src/platform/macos/nv12_zero_device.cpp
 * @brief Definitions for NV12 zero copy device on macOS.
 */
// standard includes
#include <utility>

// local includes
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/video.h"

extern "C" {
#include "libavutil/imgutils.h"
}

namespace platf {

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   *
   * @param opaque Opaque user pointer provided to the callback.
   * @param data Payload or state data to serialize, deserialize, or forward.
   */
  void free_buffer(void *opaque, uint8_t *data) {
    CVPixelBufferRelease((CVPixelBufferRef) data);
  }

  nv12_zero_device::~nv12_zero_device() {
    if (transfer_session) {
      VTPixelTransferSessionInvalidate(transfer_session);
      CFRelease(transfer_session);
    }
    if (pixel_pool) {
      CFRelease(pixel_pool);
    }
  }

  void nv12_zero_device::apply_colorspace() {
    if (!transfer_session) {
      return;
    }
    const bool bt2020 = colorspace.colorspace == video::colorspace_e::bt2020sdr ||
                        colorspace.colorspace == video::colorspace_e::bt2020;
    const bool rec601 = colorspace.colorspace == video::colorspace_e::rec601;
    const auto primaries = bt2020 ? kCVImageBufferColorPrimaries_ITU_R_2020 :
                           rec601 ? kCVImageBufferColorPrimaries_SMPTE_C :
                                    kCVImageBufferColorPrimaries_ITU_R_709_2;
    const auto matrix = bt2020 ? kCVImageBufferYCbCrMatrix_ITU_R_2020 :
                        rec601 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4 :
                                 kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    const auto transfer = colorspace.colorspace == video::colorspace_e::bt2020 ? kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ :
                          bt2020                                               ? kCVImageBufferTransferFunction_ITU_R_2020 :
                                                                                 kCVImageBufferTransferFunction_ITU_R_709_2;
    VTSessionSetProperty(transfer_session, kVTPixelTransferPropertyKey_DestinationColorPrimaries, primaries);
    VTSessionSetProperty(transfer_session, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, matrix);
    VTSessionSetProperty(transfer_session, kVTPixelTransferPropertyKey_DestinationTransferFunction, transfer);
  }

  int nv12_zero_device::convert(platf::img_t &img) {
    auto *av_img = (av_img_t *) &img;

    if (!av_img->pixel_buffer || !av_img->pixel_buffer->buf) {
      return -1;
    }

    // set_frame() must have supplied the encoder frame before any conversion
    if (!av_frame) {
      return -1;
    }

    // Release any existing CVPixelBuffer previously retained for encoding
    av_buffer_unref(&av_frame->buf[0]);

    // Attach an AVBufferRef to this frame which will retain ownership of the CVPixelBuffer
    // until av_buffer_unref() is called (above) or the frame is freed with av_frame_free().
    //
    // The presence of the AVBufferRef allows FFmpeg to simply add a reference to the buffer
    // rather than having to perform a deep copy of the data buffers in avcodec_send_frame().
    auto *retained = av_buffer_create((uint8_t *) CFRetain(av_img->pixel_buffer->buf), 0, free_buffer, nullptr, 0);
    if (!retained) {
      // Do not leak the retain we just took when the buffer could not be allocated
      CVPixelBufferRelease(av_img->pixel_buffer->buf);
      return -1;
    }
    av_frame->buf[0] = retained;
    CVPixelBufferRef output_buffer = av_img->pixel_buffer->buf;
    if (transfer_session) {
      CVPixelBufferRef converted = nullptr;
      if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pixel_pool, &converted) != kCVReturnSuccess) {
        return -1;
      }
      const auto status = VTPixelTransferSessionTransferImage(transfer_session, output_buffer, converted);
      if (status != noErr) {
        CVPixelBufferRelease(converted);
        return -1;
      }
      auto *converted_ref = av_buffer_create((uint8_t *) converted, 0, free_buffer, nullptr, 0);
      if (!converted_ref) {
        CVPixelBufferRelease(converted);
        return -1;
      }
      av_buffer_unref(&av_frame->buf[0]);
      av_frame->buf[0] = converted_ref;
      output_buffer = converted;
    }

    // Place a CVPixelBufferRef at data[3] as required by AV_PIX_FMT_VIDEOTOOLBOX
    av_frame->data[3] = (uint8_t *) output_buffer;

    return 0;
  }

  int nv12_zero_device::set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
    this->frame = frame;

    av_frame.reset(frame);

    resolution_fn(this->display, frame->width, frame->height);
    if (output_format) {
      const bool ten_bit = output_format == kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange;
      const int32_t pixel_format = colorspace.full_range ?
                                     (ten_bit ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange : kCVPixelFormatType_444YpCbCr8BiPlanarFullRange) :
                                     output_format;
      const int32_t width = frame->width;
      const int32_t height = frame->height;
      CFNumberRef format_value = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pixel_format);
      CFNumberRef width_value = CFNumberCreate(nullptr, kCFNumberSInt32Type, &width);
      CFNumberRef height_value = CFNumberCreate(nullptr, kCFNumberSInt32Type, &height);
      CFDictionaryRef surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
      const void *values[] = {format_value, width_value, height_value, surface};
      CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      const auto status = CVPixelBufferPoolCreate(nullptr, nullptr, attributes, &pixel_pool);
      CFRelease(attributes);
      CFRelease(surface);
      CFRelease(height_value);
      CFRelease(width_value);
      CFRelease(format_value);
      if (status != kCVReturnSuccess) {
        return -1;
      }
    }

    return 0;
  }

  int nv12_zero_device::init(void *display, pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    if (pix_fmt == pix_fmt_e::yuv444p || pix_fmt == pix_fmt_e::yuv444p16) {
      const bool ten_bit = pix_fmt == pix_fmt_e::yuv444p16;
      output_format = ten_bit ? kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange;
      if (VTPixelTransferSessionCreate(nullptr, &transfer_session) != noErr) {
        return -1;
      }
      pixel_format_fn(display, ten_bit ? kCVPixelFormatType_ARGB2101010LEPacked : kCVPixelFormatType_32BGRA);
    } else {
      pixel_format_fn(display, pix_fmt == pix_fmt_e::nv12 ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
    }

    this->display = display;
    this->resolution_fn = std::move(resolution_fn);

    // we never use this pointer, but its existence is checked/used
    // by the platform independent code
    data = this;

    return 0;
  }

}  // namespace platf
