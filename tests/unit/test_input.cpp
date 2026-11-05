/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/apple_pencil_side_buttons.h"
#include "src/mac_trackpad_gesture.h"
#include "src/pen_barrel_roll.h"
#include "src/platform/virtualhid_input.h"
#include "src/utility.h"

#ifdef __APPLE__
  #include "src/platform/macos/native_touch_workaround.h"
#endif

namespace {
  /**
   * @brief Protocol metadata for a fixed-size input packet.
   */
  struct input_packet_spec_t {
    std::uint32_t magic;  ///< Packet type identifier.
    std::size_t size;  ///< Complete fixed packet size.
  };

  /**
   * @brief Create raw input bytes with a caller-selected header and buffer size.
   *
   * @param magic Packet type identifier.
   * @param declared_size Packet size declared after the size field.
   * @param actual_size Number of bytes available in the packet buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  /**
   * @brief Fixture that installs a fake global virtual input backend.
   */
  class InputGamepadSessionTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve configuration and install observable fake devices.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.gamepad = "xseries";

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
    }

    /**
     * @brief Destroy retained test sessions and restore configuration.
     */
    void TearDown() override {
      input::testing::set_pen_sink({});
      input::terminate_gamepads();
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }

    /**
     * @brief Access the fake runtime installed for the current test.
     *
     * @return Fake libvirtualhid runtime.
     */
    lvh::Runtime &runtime() const {
      return *runtime_;
    }

    /**
     * @brief Access the shared libvirtualhid input context installed for the test.
     *
     * @return Fake input context.
     */
    platf::virtualhid::input_context_t &context() const {
      return *context_;
    }

  private:
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    lvh::Runtime *runtime_ = nullptr;  ///< Fake runtime installed in the global input backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "declared_size=" << declared_size;
  }

  const auto truncated_packet = make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet));

  const auto overflowing_size = make_input_packet(
    unknown_magic,
    std::numeric_limits<std::uint32_t>::max(),
    sizeof(NV_INPUT_HEADER)
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(overflowing_size));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  const auto empty_packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(empty_packet));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  const auto maximum_packet = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size,
    sizeof(std::uint32_t) + maximum_declared_size
  );
  EXPECT_TRUE(input::testing::is_valid_input_packet(maximum_packet));

  const auto oversized_text = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size + 1,
    sizeof(std::uint32_t) + maximum_declared_size + 1
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_text));

  const auto truncated_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_text));
}

/** @brief Apple Pencil gesture packets require exact wire and declared lengths. */
TEST(InputPacketValidationTest, ValidatesApplePencilSideButtonPacketSize) {
  using namespace input::apple_pencil_wire;
  constexpr auto size = sizeof(packet_t) - sizeof(std::uint32_t);
  EXPECT_TRUE(input::testing::is_valid_input_packet(make_input_packet(magic, size, sizeof(packet_t))));
  EXPECT_FALSE(input::testing::is_valid_input_packet(make_input_packet(magic, size, sizeof(packet_t) - 1)));
  EXPECT_FALSE(input::testing::is_valid_input_packet(make_input_packet(magic, size, sizeof(packet_t) + 1)));
  EXPECT_FALSE(input::testing::is_valid_input_packet(make_input_packet(magic, size - 1, sizeof(packet_t))));
}

/** @brief Magnify and rotate use the same packet layout and length. */
TEST(InputPacketValidationTest, ValidatesMacTrackpadGesturePacketSizes) {
  using namespace input::mac_trackpad_wire;
  constexpr auto size = sizeof(packet_t) - sizeof(std::uint32_t);
  for (const auto magic : {magnify_magic, rotate_magic}) {
    EXPECT_TRUE(input::testing::is_valid_input_packet(make_input_packet(magic, size, sizeof(packet_t))));
    EXPECT_FALSE(input::testing::is_valid_input_packet(make_input_packet(magic, size, sizeof(packet_t) - 1)));
    EXPECT_FALSE(input::testing::is_valid_input_packet(make_input_packet(magic, size - 1, sizeof(packet_t))));
  }
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));
    const auto valid_packet = make_input_packet(magic, declared_size, size);
    EXPECT_TRUE(input::testing::is_valid_input_packet(valid_packet)) << "magic=" << magic;

    const auto trailing_bytes = make_input_packet(magic, declared_size, size + 8);
    EXPECT_TRUE(input::testing::is_valid_input_packet(trailing_bytes)) << "magic=" << magic;

    const auto wrong_declared_size = make_input_packet(magic, declared_size - 1, size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(wrong_declared_size)) << "magic=" << magic;

    const auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_declared_size)) << "magic=" << magic;

    const auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet)) << "magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  const auto packet = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(packet));
}

TEST_F(InputGamepadSessionTest, RejectsMalformedBatchablePacketsAtQueueIngress) {
  ASSERT_FALSE(task_pool.running());
  const std::shared_ptr<input::input_t> empty_input;
  EXPECT_EQ(input::testing::queued_input_packet_count(empty_input), 0);

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "packet-validation-client");
  ASSERT_NE(stream_input, nullptr);

  auto short_header = make_input_packet(SS_PEN_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER) - 1);
  input::passthrough(stream_input, std::move(short_header));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  constexpr std::array batchable_packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
  };

  std::size_t valid_packet_count = 0;
  for (const auto &[magic, size] : batchable_packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));

    auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    input::passthrough(stream_input, std::move(truncated_packet));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto valid_packet = make_input_packet(magic, declared_size, size);
    input::passthrough(stream_input, std::move(valid_packet));
    ++valid_packet_count;
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    input::passthrough(stream_input, std::move(oversized_declared_size));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;
  }
}

TEST_F(InputGamepadSessionTest, ReusesGamepadsAcrossPauseAndDestroysThemOnTermination) {
  const std::string session_id = "paired-client-certificate";
  auto first_mail = std::make_shared<safe::mail_raw_t>();
  auto first = input::alloc(first_mail, session_id);
  ASSERT_NE(first, nullptr);
  const auto active_devices_before_gamepad = runtime().active_device_count();

  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const auto original_id = input::testing::alloc_gamepad(first, 0, metadata);
  ASSERT_GE(original_id, 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  const std::weak_ptr<input::input_t> paused = first;
  first.reset();
  EXPECT_FALSE(paused.expired());
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  auto resumed = input::alloc(resumed_mail, session_id);
  ASSERT_NE(resumed, nullptr);
  EXPECT_EQ(paused.lock(), resumed);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), original_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  input::terminate_gamepads(session_id);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), -1);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);

  auto replacement = input::alloc(std::make_shared<safe::mail_raw_t>(), session_id);
  EXPECT_NE(replacement, resumed);
}

TEST_F(InputGamepadSessionTest, RefreshesSharedVirtualInputAfterLicenseStateChanges) {
  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  const auto original_keyboard_id = context().keyboard->device_id();
  const auto original_mouse_id = context().mouse->device_id();
  const auto active_devices = runtime().active_device_count();

  input::refresh_virtual_input();

  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  EXPECT_NE(context().keyboard->device_id(), original_keyboard_id);
  EXPECT_NE(context().mouse->device_id(), original_mouse_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices);
}

/** @brief The Foundation extension accepts exactly its 40-byte layout. */
TEST(InputPacketValidationTest, ValidatesIndependentBarrelRollLayout) {
  using namespace input::pen_wire;
  for (std::size_t size = 0; size <= 48; ++size) {
    const auto packet = make_input_packet(magic, 36, size);
    EXPECT_EQ(input::testing::is_valid_input_packet(packet), size == sizeof(packet_t)) << size;
  }
  for (std::uint32_t declared = 0; declared <= 44; ++declared) {
    EXPECT_EQ(input::testing::is_valid_input_packet(make_input_packet(magic, declared, 40)), declared == 36) << declared;
  }
  EXPECT_EQ(decode_roll(util::endian::little<std::uint16_t>(725)), 5);
  EXPECT_EQ(decode_roll(unknown), unknown);
}

/** @brief Fixed, missing, varied and reset roll observations match the Foundation fork. */
TEST(PenBarrelRollTest, HardwareDetectionAndFallback) {
  using namespace input::pen_wire;
  roll_probe_t probe;
  EXPECT_EQ(probe.select(unknown, 10), 10);
  EXPECT_EQ(probe.select(0, 20), 20);
  EXPECT_EQ(probe.select(0, 30), 30);
  EXPECT_EQ(probe.select(unknown, 40), 40);
  EXPECT_EQ(probe.select(1, 50), 1);
  EXPECT_EQ(probe.select(0, 60), 0);
  EXPECT_EQ(probe.select(90, 60), 90);
  EXPECT_EQ(probe.select(180, 60), 180);
  EXPECT_EQ(probe.select(270, 60), 270);
  EXPECT_EQ(probe.select(359, 60), 359);
  EXPECT_EQ(probe.select(unknown, 70), 70);
  probe.reset();
  EXPECT_EQ(probe.select(90, 80), 80);
  EXPECT_EQ(probe.select(90, unknown), unknown);
}

namespace {
  /**
   * @brief Encode an extended pen packet with independent roll and tip azimuth.
   * @param roll Barrel angle.
   * @param azimuth Pen-tip orientation.
   * @param type Protocol transition.
   * @return Exact Foundation wire bytes.
   */
  std::vector<std::uint8_t> extended_pen(std::uint16_t roll, std::uint16_t azimuth, std::uint8_t type = LI_TOUCH_EVENT_MOVE) {
    input::pen_wire::packet_t packet {};
    packet.pen.header = {util::endian::big<std::uint32_t>(36), util::endian::little(input::pen_wire::magic)};
    packet.pen.eventType = type;
    packet.pen.toolType = LI_TOOL_TYPE_PEN;
    packet.pen.rotation = util::endian::little(azimuth);
    packet.barrelRoll = util::endian::little(roll);
    const float half = .5F;
    std::memcpy(packet.pen.x, &half, sizeof(half));
    std::memcpy(packet.pen.y, &half, sizeof(half));
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }
}  // namespace

/** @brief Preserve macOS pen samples and reset roll detection on legacy packets. */
TEST_F(InputGamepadSessionTest, BarrelRollAcrossQueuedSamplesAndProtocolChanges) {
  ASSERT_FALSE(task_pool.running());
  config::input.mouse = config::input.native_pen_touch = true;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(mail, "barrel-roll-client");
  mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {{0, 0, 1000, 500, 1000, 500}, 1000, 500, 0, 0, 1, 1, 1000, 500});
  std::vector<platf::pen_input_t> delivered;
  input::testing::set_pen_sink([&](const auto &, const auto &pen) {
    delivered.push_back(pen);
  });
  // Reject invalid extensions before either queueing or observing their roll values.
  for (int i = 0; i < 2; ++i) {
    auto malformed = extended_pen(100 + i, 45);
    malformed.push_back(0);
    input::passthrough(stream_input, std::move(malformed));
  }
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);
  input::passthrough(stream_input, extended_pen(0, 45));
  input::passthrough(stream_input, extended_pen(1, 90));
  input::passthrough(stream_input, extended_pen(0, 180));
#ifdef __APPLE__
  input::testing::process_next_input(stream_input);
  ASSERT_EQ(delivered.size(), 1);
  EXPECT_EQ(delivered.back().rotation, 45);
  EXPECT_EQ(delivered.back().barrelRoll, 45);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 2);
  input::testing::process_next_input(stream_input);
  ASSERT_EQ(delivered.size(), 2);
  EXPECT_EQ(delivered.back().rotation, 90);
  EXPECT_EQ(delivered.back().barrelRoll, 1);
  input::testing::process_next_input(stream_input);
  ASSERT_EQ(delivered.size(), 3);
#else
  input::testing::process_next_input(stream_input);
  ASSERT_EQ(delivered.size(), 1);
#endif
  EXPECT_EQ(delivered.back().rotation, 180);
  EXPECT_EQ(delivered.back().barrelRoll, 0);  // The intermediate 1 proved real roll.
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  input::passthrough(stream_input, extended_pen(90, 45));
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().barrelRoll, 90);
  EXPECT_EQ(delivered.back().rotation, 45);

  auto legacy = extended_pen(0, 270);
  legacy.resize(sizeof(SS_PEN_PACKET));
  NV_INPUT_HEADER header {util::endian::big<std::uint32_t>(32), util::endian::little<std::uint32_t>(SS_PEN_MAGIC)};
  std::memcpy(legacy.data(), &header, sizeof(header));
#ifdef __APPLE__
  auto earlier_legacy = extended_pen(0, 135);
  earlier_legacy.resize(sizeof(SS_PEN_PACKET));
  std::memcpy(earlier_legacy.data(), &header, sizeof(header));
  input::passthrough(stream_input, std::move(earlier_legacy));
#endif
  input::passthrough(stream_input, std::move(legacy));
#ifdef __APPLE__
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().barrelRoll, 135);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);
#endif
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().barrelRoll, 270);
  input::passthrough(stream_input, extended_pen(0, 30));
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().barrelRoll, 30);
  input::passthrough(stream_input, extended_pen(input::pen_wire::unknown, 60));
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().barrelRoll, 60);

  // Different transitions and tools must remain separate during coalescing.
  input::passthrough(stream_input, extended_pen(20, 90, LI_TOUCH_EVENT_DOWN));
  input::passthrough(stream_input, extended_pen(30, 90, LI_TOUCH_EVENT_UP));
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().eventType, LI_TOUCH_EVENT_DOWN);
  EXPECT_EQ(delivered.back().barrelRoll, 20);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);
  input::testing::process_next_input(stream_input);
  EXPECT_EQ(delivered.back().eventType, LI_TOUCH_EVENT_UP);
  EXPECT_EQ(delivered.back().barrelRoll, 30);
  input::testing::set_pen_sink({});
}

/** @brief Reject stale input after disconnect and restart roll detection on session resume. */
TEST_F(InputGamepadSessionTest, DisconnectClearsQueuedPenAndRollDetection) {
  ASSERT_FALSE(task_pool.running());
  config::input.mouse = true;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(mail, "roll-reset-client");
  input::passthrough(stream_input, extended_pen(0, 90));
  input::passthrough(stream_input, extended_pen(1, 90));
  input::reset(stream_input);
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);
  input::passthrough(stream_input, extended_pen(2, 90));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);
  auto resumed = input::alloc(mail, "roll-reset-client");
  EXPECT_EQ(resumed, stream_input);
  mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {{0, 0, 1000, 500, 1000, 500}, 1000, 500, 0, 0, 1, 1, 1000, 500});
  std::uint16_t roll = 0;
  input::testing::set_pen_sink([&](const auto &, const auto &pen) {
    roll = pen.barrelRoll;
  });
  input::passthrough(resumed, extended_pen(0, 120));
  input::testing::process_next_input(resumed);
  EXPECT_EQ(roll, 120);
  input::testing::set_pen_sink({});
}

#ifdef __APPLE__
/** @brief Cancellation and button-only packets reach Quartz even before touch-port metadata exists. */
TEST_F(InputGamepadSessionTest, PenControlPacketsDoNotRequireCoordinates) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "pen-controls-client");
  std::vector<platf::pen_input_t> delivered;
  input::testing::set_pen_sink([&](const auto &, const auto &pen) {
    delivered.push_back(pen);
  });
  for (const auto type : {LI_TOUCH_EVENT_BUTTON_ONLY, LI_TOUCH_EVENT_CANCEL, LI_TOUCH_EVENT_CANCEL_ALL, LI_TOUCH_EVENT_HOVER_LEAVE}) {
    input::passthrough(stream_input, extended_pen(input::pen_wire::unknown, LI_ROT_UNKNOWN, type));
    input::testing::process_next_input(stream_input);
    ASSERT_FALSE(delivered.empty());
    EXPECT_EQ(delivered.back().eventType, type);
  }
  input::testing::set_pen_sink({});
}
#endif

#ifdef __APPLE__
TEST(NativeTouchWorkaroundTest, RequiresNearbyTapsWithinTheDoubleClickWindow) {
  using namespace std::chrono_literals;
  using platf::macos::touch_is_double_tap;
  EXPECT_TRUE(touch_is_double_tap({100, 100}, {112, 105}, {1000, 500}, 250ms));
  EXPECT_FALSE(touch_is_double_tap({100, 100}, {112, 105}, {1000, 500}, 251ms));
  EXPECT_FALSE(touch_is_double_tap({100, 100}, {100, 115}, {1000, 500}, 100ms));
}

TEST(NativeTouchWorkaroundTest, UsesSeparateTapAndNormalizedDragThresholds) {
  EXPECT_TRUE(platf::macos::touch_is_stationary({0, 0}, {3, 3}));
  EXPECT_FALSE(platf::macos::touch_is_stationary({0, 0}, {5, 0}));
  EXPECT_FALSE(platf::macos::touch_exceeds_drag_threshold({0, 0}, {19, 0}, {1000, 500}));
  EXPECT_TRUE(platf::macos::touch_exceeds_drag_threshold({0, 0}, {0, 11}, {1000, 500}));
}

TEST(NativeTouchWorkaroundTest, ClassifiesPinchAndDominantScrollAxis) {
  using namespace platf::macos;
  auto mode = touch_gesture_mode::undecided;
  const auto horizontal = classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {4, 1}, {24, 1}), 0.01, mode);
  EXPECT_EQ(horizontal.x, 4);
  EXPECT_EQ(horizontal.y, 0);
  EXPECT_EQ(horizontal.magnification, 0);

  mode = touch_gesture_mode::undecided;
  const auto vertical = classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {1, -5}, {21, -5}), 0.01, mode);
  EXPECT_EQ(vertical.x, 0);
  EXPECT_EQ(vertical.y, -5);
  EXPECT_EQ(vertical.magnification, 0);

  mode = touch_gesture_mode::undecided;
  const auto pinch = classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {4, 1}, {16, 1}), 0.01, mode);
  EXPECT_EQ(pinch.x, 0);
  EXPECT_EQ(pinch.y, 0);
  EXPECT_EQ(pinch.magnification, -8);
}
TEST(NativeTouchWorkaroundTest, RequiresFixedPinchSpeedAdvantage) {
  using namespace platf::macos;
  constexpr double sample_seconds = 0.02;
  const auto threshold = magnify_speed_difference * sample_seconds;
  auto mode = touch_gesture_mode::undecided;
  classify_touch_motion({10, 0, 10 + threshold * 0.9}, sample_seconds, mode);
  EXPECT_EQ(mode, touch_gesture_mode::horizontal);

  mode = touch_gesture_mode::undecided;
  classify_touch_motion({10, 0, 10 + threshold * 1.1}, sample_seconds, mode);
  EXPECT_EQ(mode, touch_gesture_mode::magnify);
}

TEST(NativeTouchWorkaroundTest, OpposingSeparationUpdatesCancelAcrossTheWindow) {
  using namespace platf::macos;
  const auto first = measure_touch_motion({0, 0}, {20, 0}, {10, 0}, {20, 0});
  const auto second = measure_touch_motion({10, 0}, {20, 0}, {10, 0}, {30, 0});
  auto mode = touch_gesture_mode::undecided;
  classify_touch_motion({first.x + second.x, first.y + second.y, first.magnification + second.magnification}, 0.035, mode);
  EXPECT_EQ(mode, touch_gesture_mode::horizontal);
}

TEST(NativeTouchWorkaroundTest, FixedSpeedComparisonIsIndependentOfSampleDuration) {
  using namespace platf::macos;
  for (const double seconds : {1.0 / 60, 1.0 / 120}) {
    for (const double advantage : {0.9, 1.1}) {
      auto mode = touch_gesture_mode::undecided;
      constexpr double midpoint_speed = 100;
      classify_touch_motion({
        midpoint_speed * seconds,
        0,
        (midpoint_speed + magnify_speed_difference * advantage) * seconds,
      }, seconds, mode);
      EXPECT_EQ(mode, advantage > 1 ? touch_gesture_mode::magnify : touch_gesture_mode::horizontal);
    }
  }
}

TEST(NativeTouchWorkaroundTest, PreservesLockedModeWhenMotionChanges) {
  using namespace platf::macos;
  auto mode = touch_gesture_mode::undecided;
  classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {0, 0}, {20, 0}), 0.01, mode);
  EXPECT_EQ(mode, touch_gesture_mode::undecided);

  classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {4, 1}, {24, 1}), 0.01, mode);
  ASSERT_EQ(mode, touch_gesture_mode::horizontal);
  const auto scroll = classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {4, 10}, {18, 10}), 0.01, mode);
  EXPECT_EQ(scroll.x, 1);
  EXPECT_EQ(scroll.y, 0);
  EXPECT_EQ(scroll.magnification, 0);

  mode = touch_gesture_mode::undecided;
  classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {-3, 0}, {23, 0}), 0.01, mode);
  ASSERT_EQ(mode, touch_gesture_mode::magnify);
  const auto pinch = classify_touch_motion(measure_touch_motion({0, 0}, {20, 0}, {3, 10}, {24, 10}), 0.01, mode);
  EXPECT_EQ(pinch.x, 0);
  EXPECT_EQ(pinch.y, 0);
  EXPECT_EQ(pinch.magnification, 1);
}
#endif
