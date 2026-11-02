/**
 * @file src/platform/macos/pen.cpp
 * @brief Native Quartz pen pressure, tilt, eraser, proximity, and independent barrel roll.
 */
#include "pen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <moonlight-common-c/src/Limelight.h>
#include <mutex>
#include <numbers>
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
  }  // namespace

  /** @brief Serialized state of one virtual tablet tool. */
  struct pen_device_t::state_t {
    event_sink_t sink;  ///< Synchronous native poster or test recorder.
    std::mutex mutex;  ///< Serialize updates and cleanup.
    std::uint32_t device_id = next_device_id.fetch_add(1);  ///< Shared point/proximity identity.
    std::uint32_t stroke_id = 0;  ///< Mouse event number retained until tip release.
    bool in_range = false;  ///< Whether proximity entry has been delivered.
    bool down = false;  ///< Whether the tip is touching the desktop.
    bool eraser = false;  ///< Current pointing-device type.
    CGPoint position {};  ///< Last valid desktop point, also used for cancellation.
    double pressure = 0;  ///< Current contact pressure.
    double tilt_x = 0;  ///< Native horizontal tilt, normalized to [-1, 1].
    double tilt_y = 0;  ///< Native vertical tilt, normalized to [-1, 1].
    double roll = 0;  ///< Resolved barrel rotation in degrees.
    std::uint8_t buttons = 0;  ///< Protocol side-button mask.

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
     * @brief Emit a tablet point attached to a normal AppKit pointer event.
     * @param type Hover, tip down, drag, or tip up mouse event.
     */
    void point(CGEventType type) {
      auto event = CGEventCreateMouseEvent(nullptr, type, position, kCGMouseButtonLeft);
      if (!event) {
        return;
      }
      // Set subtype first: Quartz otherwise discards tablet-specific fields.
      CGEventSetIntegerValueField(event, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
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

  void pen_device_t::update(const touch_port_t &viewport, const pen_input_t &pen) {
    std::lock_guard lock {state_->mutex};
    auto &s = *state_;
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        if (s.in_range) {
          s.buttons = pen.penButtons & (LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY | LI_PEN_BUTTON_TERTIARY);
          s.point(s.down ? kCGEventLeftMouseDragged : kCGEventMouseMoved);
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
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
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
    // Captured frame pixels differ from Quartz points on Retina displays. Resolve the
    // actual capture display, including virtual and non-primary screens, at delivery time.
    auto bounds = CGRectMake(viewport.offset_x, viewport.offset_y, viewport.width, viewport.height);
    if (viewport.native_display_id != 0) {
      bounds = CGDisplayBounds(viewport.native_display_id);
      if (CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
        s.cancel();
        return;
      }
    }
    s.position = CGPointMake(bounds.origin.x + unit(pen.x) * std::max<CGFloat>(bounds.size.width - 1, 0), bounds.origin.y + unit(pen.y) * std::max<CGFloat>(bounds.size.height - 1, 0));
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
    CGEventType type = kCGEventMouseMoved;
    if (contact && !s.down) {
      s.stroke_id = next_stroke_id.fetch_add(1);
      type = kCGEventLeftMouseDown;
    } else if (contact) {
      type = kCGEventLeftMouseDragged;
    } else if (s.down) {
      type = kCGEventLeftMouseUp;
    }
    s.down = contact;
    s.point(type);
  }
}  // namespace platf::macos
