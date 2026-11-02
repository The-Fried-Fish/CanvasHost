/**
 * @file tests/unit/platform/macos/test_pen.cpp
 * @brief Inspect production Quartz tablet events without posting input to the host.
 */
#ifdef __APPLE__
  #include "../../../tests_common.h"
  #include "src/platform/macos/pen.h"
  #include "src/platform/virtualhid_input.h"

  #include <cmath>
  #include <IOKit/hidsystem/IOLLEvent.h>
  #include <limits>
  #include <vector>

namespace {
  /** @brief Own event copies until the device has finished cleanup. */
  class MacosPenTest: public ::testing::Test {
  protected:
    std::vector<CGEventRef> events;  ///< Copies of the actual backend's generated events.
    platf::touch_port_t viewport {-1000, 50, 1001, 501, 1001, 501};  ///< Offset logical desktop bounds.
    std::unique_ptr<platf::macos::pen_device_t> device;  ///< Production native tablet.

    /** @brief Create a device whose event sink never injects into the test host. */
    void SetUp() override {
      device = std::make_unique<platf::macos::pen_device_t>([this](CGEventRef event) {
        events.push_back(CGEventCreateCopy(event));
      });
    }

    /** @brief Finish proximity cleanup before releasing captured events. */
    void TearDown() override {
      device.reset();
      for (auto event : events) {
        CFRelease(event);
      }
    }

    /**
     * @brief Return a pen sample with independent roll and azimuth.
     * @param type Protocol transition.
     * @return Initialized sample.
     */
    platf::pen_input_t sample(std::uint8_t type = LI_TOUCH_EVENT_DOWN) {
      return {type, LI_TOOL_TYPE_PEN, 0, 45, 90, .5F, .5F, .75F, 0, 0, 270};
    }

    /**
     * @brief Read a numeric native field from the latest emitted event.
     * @param field Quartz event field.
     * @return Field value.
     */
    double field(CGEventField field) {
      return CGEventGetDoubleValueField(events.back(), field);
    }
  };
}  // namespace

/** @brief Preserve pressure and rotate the barrel independently of the pen's tilt azimuth. */
TEST_F(MacosPenTest, PressureTiltAndIndependentRoll) {
  device->update(viewport, sample());
  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDown);
  EXPECT_EQ(field(kCGMouseEventSubtype), kCGEventMouseSubtypeTabletPoint);
  // Quartz stores mouse pressure at 8-bit, tablet pressure at 16-bit precision.
  EXPECT_NEAR(field(kCGMouseEventPressure), .75, 1.0 / 255);
  EXPECT_NEAR(field(kCGTabletEventPointPressure), .75, 1.0 / 65535);
  EXPECT_NEAR(field(kCGTabletEventTiltX), -.5, 1.0 / 32767);
  EXPECT_NEAR(field(kCGTabletEventTiltY), 0, 1e-5);
  EXPECT_NEAR(field(kCGTabletEventRotation), 90, .01);
  const auto position = CGEventGetLocation(events.back());
  EXPECT_DOUBLE_EQ(position.x, -500);
  EXPECT_DOUBLE_EQ(position.y, 300);
  const auto id = CGEventGetIntegerValueField(events.back(), kCGTabletEventDeviceID);
  EXPECT_EQ(id, CGEventGetIntegerValueField(events.front(), kCGTabletProximityEventDeviceID));
  EXPECT_TRUE(CGEventGetIntegerValueField(events.front(), kCGTabletProximityEventCapabilityMask) & NX_TABLET_CAPABILITY_ROTATIONMASK);
  EXPECT_EQ(CGEventGetType(events[1]), kCGEventTabletProximity);
}

/** @brief Legacy clients derive barrel rotation from azimuth while unknown angles clear stale values. */
TEST_F(MacosPenTest, LegacyFallbackAndUnknownMeasurements) {
  auto pen = sample();
  pen.barrelRoll = LI_ROT_UNKNOWN;
  device->update(viewport, pen);
  EXPECT_NEAR(field(kCGTabletEventRotation), 270, .01);
  pen.eventType = LI_TOUCH_EVENT_MOVE;
  pen.rotation = LI_ROT_UNKNOWN;
  pen.tilt = LI_TILT_UNKNOWN;
  device->update(viewport, pen);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventRotation), 0);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventTiltX), 0);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventTiltY), 0);
}

TEST_F(MacosPenTest, ReversesHoverRollAndAzimuthFallback) {
  for (const auto type : {LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_DOWN, LI_TOUCH_EVENT_MOVE}) {
    for (const auto angle : {0, 1, 90, 180, 270, 359}) {
      auto pen = sample(type);
      pen.rotation = 90;
      pen.barrelRoll = angle;
      device->update(viewport, pen);
      EXPECT_NEAR(field(kCGTabletEventRotation), (360 - angle) % 360, .01);
      EXPECT_NEAR(field(kCGTabletEventTiltX), -.5, 1.0 / 32767);
      device->update({}, sample(LI_TOUCH_EVENT_CANCEL_ALL));
    }
  }
  auto hover = sample(LI_TOUCH_EVENT_HOVER);
  hover.barrelRoll = LI_ROT_UNKNOWN;
  hover.rotation = 90;
  device->update(viewport, hover);
  EXPECT_NEAR(field(kCGTabletEventRotation), 270, .01);
  EXPECT_NEAR(field(kCGTabletEventTiltX), -.5, 1.0 / 32767);
  hover.rotation = LI_ROT_UNKNOWN;
  device->update(viewport, hover);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventRotation), 0);
}

/** @brief Tip-up preserves hover; leaving range and repeated cancellation clean up exactly once. */
TEST_F(MacosPenTest, StrokeLifecycleAndCancelWithoutCoordinates) {
  auto pen = sample(LI_TOUCH_EVENT_HOVER);
  device->update(viewport, pen);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventMouseMoved);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventPointPressure), 0);
  pen.eventType = LI_TOUCH_EVENT_DOWN;
  device->update(viewport, pen);
  const auto stroke = field(kCGMouseEventNumber);
  pen.eventType = LI_TOUCH_EVENT_MOVE;
  device->update(viewport, pen);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDragged);
  EXPECT_EQ(field(kCGMouseEventNumber), stroke);
  pen.eventType = LI_TOUCH_EVENT_UP;
  device->update(viewport, pen);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseUp);
  EXPECT_EQ(field(kCGMouseEventNumber), stroke);
  EXPECT_EQ(field(kCGTabletEventPointButtons), 0);
  EXPECT_EQ(field(kCGTabletEventPointPressure), 0);
  auto count = events.size();
  pen.eventType = LI_TOUCH_EVENT_HOVER;
  device->update(viewport, pen);
  EXPECT_EQ(events.size(), count + 1);
  pen.eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
  device->update({}, pen);
  EXPECT_EQ(field(kCGTabletProximityEventEnterProximity), 0);
  count = events.size();
  pen.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  device->update({}, pen);
  EXPECT_EQ(events.size(), count);
  pen.eventType = LI_TOUCH_EVENT_UP;
  device->update(viewport, pen);
  EXPECT_EQ(events.size(), count);
}

TEST_F(MacosPenTest, HoverLeaveDoesNotSendARotationlessMouseMove) {
  auto hover = sample(LI_TOUCH_EVENT_HOVER);
  hover.barrelRoll = 135;
  device->update(viewport, hover);
  EXPECT_NEAR(field(kCGTabletEventRotation), 225, .01);
  const auto count = events.size();
  device->update({}, sample(LI_TOUCH_EVENT_HOVER_LEAVE));
  ASSERT_EQ(events.size(), count + 1);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventTabletProximity);
  EXPECT_EQ(field(kCGTabletProximityEventEnterProximity), 0);
  device->update({}, sample(LI_TOUCH_EVENT_HOVER_LEAVE));
  EXPECT_EQ(events.size(), count + 1);
  device->update(viewport, hover);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventMouseMoved);
  EXPECT_NEAR(field(kCGTabletEventRotation), 225, .01);
}

TEST_F(MacosPenTest, IgnoresHoverUntilTheStrokeEnds) {
  device->update(viewport, sample());
  const auto count = events.size();
  const auto stroke = field(kCGMouseEventNumber);
  auto hover = sample(LI_TOUCH_EVENT_HOVER);
  hover.x = .9F;
  hover.y = .9F;
  hover.pressureOrDistance = 0;
  hover.tilt = 0;
  hover.barrelRoll = 15;
  hover.toolType = LI_TOOL_TYPE_ERASER;
  device->update(viewport, hover);
  EXPECT_EQ(events.size(), count);

  device->update({}, sample(LI_TOUCH_EVENT_BUTTON_ONLY));
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDragged);
  EXPECT_EQ(field(kCGMouseEventNumber), stroke);
  EXPECT_DOUBLE_EQ(CGEventGetLocation(events.back()).x, -500);
  EXPECT_NEAR(field(kCGTabletEventPointPressure), .75, 1.0 / 65535);
  EXPECT_NEAR(field(kCGTabletEventTiltX), -.5, 1.0 / 32767);
  EXPECT_NEAR(field(kCGTabletEventRotation), 90, .01);

  device->update(viewport, sample(LI_TOUCH_EVENT_MOVE));
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDragged);
  EXPECT_EQ(field(kCGMouseEventNumber), stroke);
  device->update(viewport, sample(LI_TOUCH_EVENT_UP));
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseUp);
  EXPECT_EQ(field(kCGMouseEventNumber), stroke);

  hover.toolType = LI_TOOL_TYPE_PEN;
  auto before_hover = events.size();
  device->update(viewport, hover);
  EXPECT_EQ(events.size(), before_hover + 1);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventMouseMoved);
  EXPECT_NEAR(CGEventGetLocation(events.back()).x, -100, .001);
  EXPECT_NEAR(field(kCGTabletEventRotation), 345, .01);

  device->update(viewport, sample());
  device->update({}, sample(LI_TOUCH_EVENT_CANCEL_ALL));
  before_hover = events.size();
  device->update(viewport, hover);
  EXPECT_EQ(events.size(), before_hover + 3);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventMouseMoved);
}

/** @brief Button-only packets retain position, pressure, tilt, and stroke identity. */
TEST_F(MacosPenTest, ButtonsAndCancellation) {
  device->update(viewport, sample());
  auto pen = sample(LI_TOUCH_EVENT_BUTTON_ONLY);
  pen.x = pen.y = std::numeric_limits<float>::quiet_NaN();
  pen.penButtons = LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_TERTIARY;
  device->update({}, pen);
  EXPECT_EQ(field(kCGTabletEventPointButtons), 1 | (pen.penButtons << 1));
  EXPECT_DOUBLE_EQ(CGEventGetLocation(events.back()).x, -500);
  EXPECT_NEAR(field(kCGTabletEventPointPressure), .75, 1.0 / 65535);
  EXPECT_NEAR(field(kCGTabletEventRotation), 90, .01);
  const auto count = events.size();
  pen.eventType = LI_TOUCH_EVENT_CANCEL;
  device->update({}, pen);
  ASSERT_EQ(events.size(), count + 2);
  EXPECT_EQ(CGEventGetType(events[count]), kCGEventLeftMouseUp);
  EXPECT_EQ(CGEventGetIntegerValueField(events[count], kCGTabletEventPointButtons), 0);
  EXPECT_EQ(CGEventGetDoubleValueField(events[count], kCGTabletEventPointPressure), 0);
  EXPECT_EQ(field(kCGTabletProximityEventEnterProximity), 0);
}

/** @brief Switching to eraser releases the pen first and changes proximity pointer type. */
TEST_F(MacosPenTest, EraserAndUnknownTool) {
  device->update(viewport, sample());
  auto pen = sample(LI_TOUCH_EVENT_MOVE);
  pen.toolType = LI_TOOL_TYPE_ERASER;
  device->update(viewport, pen);
  ASSERT_EQ(events.size(), 8);
  EXPECT_EQ(CGEventGetType(events[3]), kCGEventLeftMouseUp);
  EXPECT_EQ(CGEventGetIntegerValueField(events[5], kCGTabletProximityEventPointerType), NX_TABLET_POINTER_ERASER);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDown);
  pen.toolType = LI_TOOL_TYPE_UNKNOWN;
  device->update(viewport, pen);
  EXPECT_EQ(events.size(), 9);
}

/** @brief Reject invalid geometry, clamp measurements, and still release a malformed tip-up. */
TEST_F(MacosPenTest, SanitizesUntrustedSamples) {
  auto pen = sample();
  device->update({}, pen);
  EXPECT_TRUE(events.empty());
  pen.x = std::numeric_limits<float>::infinity();
  device->update(viewport, pen);
  EXPECT_TRUE(events.empty());
  pen.x = 2;
  pen.y = -1;
  pen.pressureOrDistance = 5;
  pen.barrelRoll = 725;
  pen.tilt = 200;
  device->update(viewport, pen);
  EXPECT_DOUBLE_EQ(CGEventGetLocation(events.back()).x, 0);
  EXPECT_DOUBLE_EQ(CGEventGetLocation(events.back()).y, 50);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventPointPressure), 1);
  EXPECT_NEAR(field(kCGTabletEventRotation), 355, .01);
  EXPECT_GE(field(kCGTabletEventTiltX), -1);
  EXPECT_LE(field(kCGTabletEventTiltY), 1);
  pen.eventType = LI_TOUCH_EVENT_MOVE;
  pen.pressureOrDistance = std::numeric_limits<float>::quiet_NaN();
  device->update(viewport, pen);
  EXPECT_DOUBLE_EQ(field(kCGTabletEventPointPressure), 0);
  const auto count = events.size();
  pen.eventType = 255;
  device->update(viewport, pen);
  EXPECT_EQ(events.size(), count);
  pen.eventType = LI_TOUCH_EVENT_UP;
  device->update({}, pen);
  EXPECT_EQ(events.size(), count + 2);
}

/** @brief Each client has an independent tablet identity and destruction releases active strokes. */
TEST_F(MacosPenTest, SeparateClientsAndDestruction) {
  device->update(viewport, sample());
  const auto id = field(kCGTabletEventDeviceID);
  {
    platf::macos::pen_device_t other {[this](CGEventRef event) {
      events.push_back(CGEventCreateCopy(event));
    }};
    other.update(viewport, sample());
    EXPECT_NE(field(kCGTabletEventDeviceID), id);
  }
  EXPECT_EQ(CGEventGetType(events[events.size() - 2]), kCGEventLeftMouseUp);
  EXPECT_EQ(field(kCGTabletProximityEventEnterProximity), 0);
  const auto count = events.size();
  device.reset();
  EXPECT_EQ(events.size(), count + 2);
}

/** @brief Release side buttons while hovering and ignore button-only packets after leaving range. */
TEST_F(MacosPenTest, HoverButtonCleanup) {
  auto pen = sample(LI_TOUCH_EVENT_BUTTON_ONLY);
  device->update({}, pen);
  EXPECT_TRUE(events.empty());
  pen.eventType = LI_TOUCH_EVENT_HOVER;
  pen.penButtons = LI_PEN_BUTTON_SECONDARY;
  device->update(viewport, pen);
  const auto count = events.size();
  pen.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  device->update({}, pen);
  ASSERT_EQ(events.size(), count + 2);
  EXPECT_EQ(CGEventGetIntegerValueField(events[count], kCGTabletEventPointButtons), 0);
}

/** @brief Resolve the actual display's logical Quartz bounds rather than using captured Retina pixels. */
TEST_F(MacosPenTest, NativeDisplayCoordinates) {
  viewport.native_display_id = CGMainDisplayID();
  const auto bounds = CGDisplayBounds(viewport.native_display_id);
  auto pen = sample();
  pen.x = pen.y = 1;
  device->update(viewport, pen);
  const auto point = CGEventGetLocation(events.back());
  EXPECT_DOUBLE_EQ(point.x, CGRectGetMaxX(bounds) - 1);
  EXPECT_DOUBLE_EQ(point.y, CGRectGetMaxY(bounds) - 1);
}

/** @brief Route native pen samples through the platform adapter and clean up when input is disabled. */
TEST_F(MacosPenTest, PlatformAdapterAndDisabledInput) {
  const auto original_config = config::input;
  const auto restore = util::fail_guard([&]() {
    config::input = original_config;
  });
  config::input.mouse = config::input.native_pen_touch = true;
  platf::virtualhid::input_context_t global {lvh::BackendKind::fake};
  platf::virtualhid::client_context_t client {global};
  EXPECT_EQ(client.macos_pen, nullptr);
  client.macos_pen = std::move(device);
  platf::virtualhid::pen_update(client, viewport, sample());
  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(CGEventGetType(events.back()), kCGEventLeftMouseDown);
  EXPECT_NEAR(field(kCGTabletEventRotation), 90, .01);
  config::input.native_pen_touch = false;
  platf::virtualhid::pen_update(client, viewport, sample());
  EXPECT_EQ(events.size(), 5);
  EXPECT_EQ(CGEventGetType(events[3]), kCGEventLeftMouseUp);
  EXPECT_EQ(field(kCGTabletProximityEventEnterProximity), 0);
  config::input.native_pen_touch = true;
  config::input.mouse = false;
  platf::virtualhid::pen_update(client, viewport, sample());
  EXPECT_EQ(events.size(), 5);
}
#endif

#ifdef __APPLE__
/** @brief Advertise the native pen only while both input configuration switches allow it. */
TEST(MacosPenBackendTest, CapabilityAndDeviceSelection) {
  const auto original_config = config::input;
  const auto restore = util::fail_guard([&]() {
    config::input = original_config;
  });
  config::input.mouse = config::input.native_pen_touch = true;
  EXPECT_TRUE(platf::get_capabilities() & platf::platform_caps::pen_touch);
  config::input.native_pen_touch = false;
  EXPECT_FALSE(platf::get_capabilities() & platf::platform_caps::pen_touch);
  config::input.mouse = false;
  config::input.native_pen_touch = true;
  EXPECT_FALSE(platf::get_capabilities() & platf::platform_caps::pen_touch);
  platf::virtualhid::input_context_t global;
  ASSERT_NE(global.runtime, nullptr);
  ASSERT_EQ(global.runtime->capabilities().backend_name, "macos-coregraphics");
  platf::virtualhid::client_context_t client {global};
  EXPECT_NE(client.macos_pen, nullptr);
  EXPECT_EQ(client.pen, nullptr);
}
#endif
