/**
 * @file src/platform/macos/pen.cpp
 * @brief Native Quartz pen pressure, tilt, eraser, proximity, and independent barrel roll.
 */
#include "pen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <moonlight-common-c/src/Limelight.h>
#include <mach/mach_time.h>
#include <mutex>
#include <numbers>
#include <time.h>
#include <utility>

namespace platf::macos {
  namespace {
    std::atomic<std::uint32_t> next_device_id {0x4c00};  ///< Distinct synthetic identities for concurrent clients.
    std::atomic<std::uint32_t> next_stroke_id {1};  ///< Stable mouse event number throughout each stroke.

    /**
     * @brief Sanitize an untrusted normalized measurement.
     * @param value Client measurement.
     * @return A finite number in [0, 1].
     */
    double unit(float value) {
      return std::isfinite(value) ? std::clamp<double>(value, 0, 1) : 0;
    }

    /** @brief Shared serialized Quartz envelope, with zeroed position and timestamps. */
    constexpr std::array<std::uint8_t, 116> gesture_header {
      0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x40, 0x35, 0x00, 0x00, 0x00, 0x03,
      0x00, 0x01, 0x40, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x37,
      0x00, 0x00, 0x00, 0x1d, 0x00, 0x02, 0xc0, 0x38, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc0, 0x39, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x3b, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x01, 0x40, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x34,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xa9, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x6a, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x01, 0x40, 0x6b, 0x00, 0x00, 0x03, 0x20,
    };
    /** @brief Embedded HID double-tap event. */
    constexpr std::array<std::uint8_t, 56> double_tap_hid {
      0x00, 0x34, 0x10, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x10, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
      0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    };
    /** @brief Embedded HID squeeze event without pressure or hover telemetry. */
    constexpr std::array<std::uint8_t, 132> squeeze_hid {
      0x00, 0x80, 0x10, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x11, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
      0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x00, 0xff, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xf0, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    /** @brief Shared Quartz gesture fields following the embedded HID record. */
    constexpr std::array<std::uint8_t, 40> gesture_fields {
      0x00, 0x01, 0x40, 0x6e, 0x00, 0x00, 0x00, 0x27, 0x00, 0x01, 0x40, 0x6f,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x70, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x01, 0x40, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x85,
      0x00, 0x00, 0x00, 0x00,
    };
  }  // namespace

  std::optional<CGPoint> pen_display_location(const touch_port_t &viewport, float x, float y) {
    if (viewport.width <= 0 || viewport.height <= 0 || !std::isfinite(x) || !std::isfinite(y)) {
      return std::nullopt;
    }
    // Captured pixels can differ from Quartz points on Retina displays.
    const auto bounds = viewport.native_display_id != 0 ?
      CGDisplayBounds(viewport.native_display_id) :
      CGRectMake(viewport.offset_x, viewport.offset_y, viewport.width, viewport.height);
    if (CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
      return std::nullopt;
    }
    return CGPointMake(bounds.origin.x + unit(x) * std::max<CGFloat>(bounds.size.width - 1, 0), bounds.origin.y + unit(y) * std::max<CGFloat>(bounds.size.height - 1, 0));
  }

  CGEventRef create_apple_pencil_gesture(input::apple_pencil_wire::event_t gesture, CGPoint position) {
    using enum input::apple_pencil_wire::event_t;
    std::uint8_t squeeze_hid_phase = 0;
    switch (gesture) {
      case double_tap:
      case squeeze_change:
        break;
      case squeeze_begin:
        squeeze_hid_phase = 1;
        break;
      case squeeze_end:
        squeeze_hid_phase = 2;
        break;
      case squeeze_cancel:
        squeeze_hid_phase = 3;
        break;
      default:
        return nullptr;
    }

    // Quartz type 29 also carries trackpad gestures. AppKit distinguishes Pencil
    // double tap and squeeze by the embedded HID record, which Quartz has no public setter for.
    std::array<std::uint8_t, gesture_header.size() + squeeze_hid.size() + gesture_fields.size()> bytes;
    auto hid_start = std::copy(gesture_header.begin(), gesture_header.end(), bytes.begin());
    auto fields_start = gesture == double_tap ?
      std::copy(double_tap_hid.begin(), double_tap_hid.end(), hid_start) :
      std::copy(squeeze_hid.begin(), squeeze_hid.end(), hid_start);
    const auto end = std::copy(gesture_fields.begin(), gesture_fields.end(), fields_start);

    constexpr auto hid_timestamp_offset = 4;
    constexpr auto hid_gesture_phase_offset = 43;
    constexpr auto hid_squeeze_phase_offset = 88;
    constexpr auto quartz_phase_offset = 31;
    const auto phase = static_cast<std::uint8_t>(gesture);
    hid_start[hid_gesture_phase_offset] = phase;
    fields_start[quartz_phase_offset] = phase;
    if (gesture != double_tap) {
      hid_start[hid_squeeze_phase_offset] = squeeze_hid_phase;
    }

    const auto hid_timestamp = mach_absolute_time();
    constexpr auto quartz_hid_timestamp_offset = 92;
    for (unsigned i = 0; i < sizeof(hid_timestamp); ++i) {
      const auto byte = static_cast<std::uint8_t>(hid_timestamp >> (i * 8));
      hid_start[hid_timestamp_offset + i] = byte;
      bytes[quartz_hid_timestamp_offset + sizeof(hid_timestamp) - 1 - i] = byte;
    }

    auto data = CFDataCreate(nullptr, bytes.data(), end - bytes.begin());
    if (!data) {
      return nullptr;
    }
    auto event = CGEventCreateFromData(nullptr, data);
    CFRelease(data);
    if (event) {
      CGEventSetLocation(event, position);
      CGEventSetTimestamp(event, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    }
    return event;
  }

  bool post_apple_pencil_gesture(input::apple_pencil_wire::event_t gesture, CGPoint position) {
    const std::unique_ptr<__CGEvent, decltype(&CFRelease)> event {create_apple_pencil_gesture(gesture, position), CFRelease};
    if (!event) {
      return false;
    }
    CGEventPost(kCGHIDEventTap, event.get());
    return true;
  }

  /** @brief Serialized state of one virtual tablet tool. */
  struct pen_device_t::state_t {
    event_sink_t sink;  ///< Synchronous native poster or test recorder.
    std::mutex mutex;  ///< Serialize updates and cleanup.
    std::uint32_t device_id = next_device_id.fetch_add(1);  ///< Shared point/proximity identity.
    std::uint32_t stroke_id = 0;  ///< Mouse event number retained until tip release.
    config::penInjectMode mode = config::default_pen_inject_mode;  ///< Mode fixed for the current stroke.
    std::chrono::steady_clock::time_point move_after {};  ///< End of the post-down startup window.
    std::atomic<std::chrono::steady_clock::time_point> drag_token {};  ///< Timestamp of the unconsumed refresh.
    CVDisplayLinkRef drag_display_link = nullptr;  ///< Refresh source for the capture display.
    CGDirectDisplayID drag_display_id = kCGNullDirectDisplay;  ///< Display bound to the refresh source.
    bool in_range = false;  ///< Whether proximity entry has been delivered.
    bool down = false;  ///< Whether the tip is touching the desktop.
    bool eraser = false;  ///< Current pointing-device type.
    CGPoint position {};  ///< Last valid desktop point, also used for cancellation.
    double pressure = 0;  ///< Current contact pressure.
    double tilt_x = 0;  ///< Native horizontal tilt, normalized to [-1, 1].
    double tilt_y = 0;  ///< Native vertical tilt, normalized to [-1, 1].
    double roll = 0;  ///< Resolved barrel rotation in degrees.
    std::uint8_t buttons = 0;  ///< Protocol side-button mask.

    /** @brief Stop refresh callbacks and clear any unconsumed drag token. */
    void stop_drag_refresh() {
      if (drag_display_link) {
        CVDisplayLinkStop(drag_display_link);
        CVDisplayLinkRelease(drag_display_link);
        drag_display_link = nullptr;
      }
      drag_display_id = kCGNullDirectDisplay;
      drag_token.store({}, std::memory_order_relaxed);
    }

    /**
     * @brief Refill the drag token from the current capture display.
     * @param display Core Graphics identifier of the capture display.
     */
    void bind_drag_refresh(CGDirectDisplayID display) {
      if (display == drag_display_id) {
        return;
      }
      stop_drag_refresh();
      drag_display_id = display;
      if (display == kCGNullDirectDisplay ||
          CVDisplayLinkCreateWithCGDisplay(display, &drag_display_link) != kCVReturnSuccess) {
        return;
      }

      auto refill = [](CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *, CVOptionFlags, CVOptionFlags *, void *context) -> CVReturn {
        using clock = std::chrono::steady_clock;
        static_cast<std::atomic<clock::time_point> *>(context)->store(clock::now(), std::memory_order_relaxed);
        return kCVReturnSuccess;
      };
      if (CVDisplayLinkSetOutputCallback(drag_display_link, refill, &drag_token) != kCVReturnSuccess ||
          CVDisplayLinkStart(drag_display_link) != kCVReturnSuccess) {
        CVDisplayLinkRelease(drag_display_link);
        drag_display_link = nullptr;
      }
    }

    /**
     * @brief Consume an eligible refresh for an arriving pen sample.
     * @param received_at Host receipt time of the sample.
     * @return Whether this sample should also be injected as a mouse drag.
     */
    bool consume_drag_refresh(std::chrono::steady_clock::time_point received_at) {
      auto token = drag_token.load(std::memory_order_relaxed);
      while (token != std::chrono::steady_clock::time_point {} && token >= move_after && token <= received_at) {
        if (drag_token.compare_exchange_weak(token, {}, std::memory_order_relaxed)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Deliver a constructed event and release its ownership.
     * @param event Owned event, or null if allocation failed.
     */
    void post(CGEventRef event) {
      if (!event) {
        return;
      }
      const std::unique_ptr<const __CGEvent, decltype(&CFRelease)> owned {event, CFRelease};
      CGEventSetFlags(event, CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState));
      sink(event);
    }

    /**
     * @brief Emit a standalone tablet sample or a mouse event with tablet data.
     * @param type Tablet sample, hover, tip down, drag, or tip up.
     */
    void point(CGEventType type) {
      const bool standalone = type == kCGEventTabletPointer;
      auto event = CGEventCreateMouseEvent(nullptr, standalone ? kCGEventMouseMoved : type, position, kCGMouseButtonLeft);
      if (!event) {
        return;
      }
      // Set subtype first: Quartz otherwise discards tablet-specific fields.
      CGEventSetIntegerValueField(event, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
      if (standalone) {
        CGEventSetType(event, kCGEventTabletPointer);
      }
      CGEventSetIntegerValueField(event, kCGTabletEventDeviceID, device_id);
      CGEventSetIntegerValueField(event, kCGTabletEventPointX, std::llround(position.x));
      CGEventSetIntegerValueField(event, kCGTabletEventPointY, std::llround(position.y));
      CGEventSetIntegerValueField(event, kCGTabletEventPointButtons, (down ? 1 : 0) | (buttons << 1));
      CGEventSetDoubleValueField(event, kCGMouseEventPressure, down ? pressure : 0);
      CGEventSetDoubleValueField(event, kCGTabletEventPointPressure, down ? pressure : 0);
      CGEventSetDoubleValueField(event, kCGTabletEventTiltX, tilt_x);
      CGEventSetDoubleValueField(event, kCGTabletEventTiltY, tilt_y);
      CGEventSetDoubleValueField(event, kCGTabletEventRotation, roll);
      CGEventSetIntegerValueField(event, kCGMouseEventNumber, stroke_id);
      CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
      post(event);
    }

    /**
     * @brief Announce or withdraw this tool using both native proximity delivery forms.
     * @param entering True for proximity entry.
     */
    void proximity(bool entering) {
      for (bool standalone : {false, true}) {
        if (!entering && !standalone) {
          continue;
        }
        auto event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, position, kCGMouseButtonLeft);
        if (!event) {
          continue;
        }
        CGEventSetIntegerValueField(event, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletProximity);
        if (standalone) {
          CGEventSetType(event, kCGEventTabletProximity);
        }
        CGEventSetIntegerValueField(event, kCGTabletProximityEventEnterProximity, entering);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventPointerType, eraser ? NX_TABLET_POINTER_ERASER : NX_TABLET_POINTER_PEN);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventDeviceID, device_id);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventPointerID, device_id);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventSystemTabletID, device_id);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventVendorUniqueID, device_id);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventVendorID, 1);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventTabletID, 1);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventCapabilityMask, NX_TABLET_CAPABILITY_DEVICEIDMASK | NX_TABLET_CAPABILITY_ABSXMASK | NX_TABLET_CAPABILITY_ABSYMASK | NX_TABLET_CAPABILITY_BUTTONSMASK | NX_TABLET_CAPABILITY_PRESSUREMASK | NX_TABLET_CAPABILITY_TILTXMASK | NX_TABLET_CAPABILITY_TILTYMASK | NX_TABLET_CAPABILITY_ROTATIONMASK);
        post(event);
      }
      in_range = entering;
    }

    /** @brief Release pressed buttons and withdraw the tool, preserving the last valid location. */
    void cancel() {
      stop_drag_refresh();
      const bool had_buttons = buttons != 0;
      buttons = 0;
      if (down) {
        down = false;
        point(kCGEventLeftMouseUp);
      } else if (in_range && had_buttons) {
        point(kCGEventMouseMoved);
      }
      if (in_range) {
        proximity(false);
      }
    }
  };

  pen_device_t::pen_device_t(event_sink_t sink):
      state_ {std::make_unique<state_t>()} {
    state_->sink = sink ? std::move(sink) : event_sink_t {[](CGEventRef event) {
      CGEventPost(kCGHIDEventTap, event);
    }};
  }

  pen_device_t::~pen_device_t() {
    std::lock_guard lock {state_->mutex};
    state_->cancel();
  }

#ifdef SUNSHINE_TESTS
  void pen_device_t::signal_display_refresh(std::chrono::steady_clock::time_point refreshed_at) {
    state_->drag_token.store(refreshed_at, std::memory_order_relaxed);
  }
#endif

  void pen_device_t::update(const touch_port_t &viewport, const pen_input_t &pen) {
    std::lock_guard lock {state_->mutex};
    auto &s = *state_;
    using clock = std::chrono::steady_clock;
    using enum config::penInjectMode;
    const auto received_at = pen.received_at == clock::time_point {} ? clock::now() : pen.received_at;
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        if (s.in_range) {
          s.buttons = pen.penButtons & (LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY | LI_PEN_BUTTON_TERTIARY);
          const auto point_type = s.mode == regular ? kCGEventLeftMouseDragged : kCGEventTabletPointer;
          s.point(s.down ? point_type : kCGEventMouseMoved);
        }
        return;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        s.cancel();
        return;
      case LI_TOUCH_EVENT_HOVER:
        if (s.down) {
          return;
        }
        break;
      case LI_TOUCH_EVENT_MOVE:
        if (s.down && s.mode != regular && received_at < s.move_after) {
          return;
        }
        break;
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_UP:
        break;
      default:
        return;
    }
    if (viewport.width <= 0 || viewport.height <= 0 || !std::isfinite(pen.x) || !std::isfinite(pen.y)) {
      // A malformed release must still end an existing stroke.
      if (pen.eventType == LI_TOUCH_EVENT_UP) {
        s.cancel();
      }
      return;
    }
    const bool eraser = pen.toolType == LI_TOOL_TYPE_UNKNOWN ? s.eraser : pen.toolType == LI_TOOL_TYPE_ERASER;
    if (s.in_range && eraser != s.eraser) {
      s.cancel();
    }
    s.eraser = eraser;
    const auto position = pen_display_location(viewport, pen.x, pen.y);
    if (!position) {
      s.cancel();
      return;
    }
    s.position = *position;
    s.pressure = unit(pen.pressureOrDistance);
    s.buttons = pen.penButtons & (LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY | LI_PEN_BUTTON_TERTIARY);
    s.tilt_x = s.tilt_y = 0;
    if (pen.tilt != LI_TILT_UNKNOWN && pen.rotation != LI_ROT_UNKNOWN) {
      const double tilt = std::min<int>(pen.tilt, 90) * std::numbers::pi / 180;
      const double azimuth = -(pen.rotation % 360) * std::numbers::pi / 180;
      const double radius = std::sin(tilt);
      const double z = std::cos(tilt);
      s.tilt_x = std::atan2(std::sin(azimuth) * radius, z) / (std::numbers::pi / 2);
      s.tilt_y = std::atan2(std::cos(azimuth) * radius, z) / (std::numbers::pi / 2);
    }
    const auto roll = pen.barrelRoll == LI_ROT_UNKNOWN ? pen.rotation : pen.barrelRoll;
    s.roll = roll == LI_ROT_UNKNOWN ? 0 : (360 - roll % 360) % 360;
    if (!s.in_range) {
      // A late release must never introduce a new tablet into proximity.
      if (pen.eventType == LI_TOUCH_EVENT_UP) {
        return;
      }
      s.proximity(true);
    }
    const bool contact = pen.eventType == LI_TOUCH_EVENT_DOWN || pen.eventType == LI_TOUCH_EVENT_MOVE;
    if (contact && !s.down) {
      s.mode = config::pen_inject_mode.load(std::memory_order_relaxed);
      s.stroke_id = next_stroke_id.fetch_add(1);
      s.down = true;
      s.point(kCGEventLeftMouseDown);
      s.move_after = clock::now() + std::chrono::microseconds {16670};
      if (s.mode == mixed) {
        s.bind_drag_refresh(viewport.native_display_id);
      }
      return;
    }
    if (contact) {
      if (s.mode == regular) {
        s.point(kCGEventLeftMouseDragged);
      } else {
        if (s.mode == mixed) {
          s.bind_drag_refresh(viewport.native_display_id);
        }
        const bool send_drag = s.mode == mixed && s.consume_drag_refresh(received_at);
        s.point(kCGEventTabletPointer);
        if (send_drag) {
          s.point(kCGEventLeftMouseDragged);
        }
      }
      return;
    }
    if (s.down) {
      s.down = false;
      s.point(kCGEventLeftMouseUp);
      s.stop_drag_refresh();
    } else {
      s.point(kCGEventMouseMoved);
    }
  }
}  // namespace platf::macos
