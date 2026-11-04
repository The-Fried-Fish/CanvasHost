/**
 * @file src/input.cpp
 * @brief Definitions for gamepad, keyboard, and mouse input handling.
 */
#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

// lib includes
#include <boost/endian/buffers.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "mac_trackpad_gesture.h"
#include "pen_barrel_roll.h"
#include "platform/common.h"
#include "platform/virtualhid_input.h"
#include "thread_pool.h"
#include "utility.h"
#ifdef __APPLE__
  #include "platform/macos/native_touch_workaround.h"
  #include "platform/macos/smooth_scroll.h"
#endif

// Win32 WHEEL_DELTA constant
#ifndef WHEEL_DELTA
constexpr int WHEEL_DELTA = 120;  ///< Standard Windows wheel delta used to normalize scroll events.
#endif

using namespace std::literals;

namespace input {

  constexpr auto MAX_GAMEPADS = std::min((std::size_t) platf::MAX_GAMEPADS, sizeof(std::int16_t) * 8);  ///< Maximum gamepads representable by the active gamepad mask.
/**
 * @def DISABLE_LEFT_BUTTON_DELAY
 * @brief Macro for DISABLE LEFT BUTTON DELAY.
 */
#define DISABLE_LEFT_BUTTON_DELAY ((thread_pool_util::ThreadPool::task_id_t) 0x01)
/**
 * @def ENABLE_LEFT_BUTTON_DELAY
 * @brief Macro for ENABLE LEFT BUTTON DELAY.
 */
#define ENABLE_LEFT_BUTTON_DELAY nullptr

  constexpr auto VKEY_SHIFT = 0x10;  ///< Windows virtual-key code for shift.
  constexpr auto VKEY_LSHIFT = 0xA0;  ///< Windows virtual-key code for lshift.
  constexpr auto VKEY_RSHIFT = 0xA1;  ///< Windows virtual-key code for rshift.
  constexpr auto VKEY_CONTROL = 0x11;  ///< Windows virtual-key code for control.
  constexpr auto VKEY_LCONTROL = 0xA2;  ///< Windows virtual-key code for lcontrol.
  constexpr auto VKEY_RCONTROL = 0xA3;  ///< Windows virtual-key code for rcontrol.
  constexpr auto VKEY_MENU = 0x12;  ///< Windows virtual-key code for menu.
  constexpr auto VKEY_LMENU = 0xA4;  ///< Windows virtual-key code for lmenu.
  constexpr auto VKEY_RMENU = 0xA5;  ///< Windows virtual-key code for rmenu.

  /**
   * @brief Enumerates supported button state options.
   */
  enum class button_state_e {
    NONE,  ///< No button state
    DOWN,  ///< Button is down
    UP  ///< Button is up
  };

  /**
   * @brief Allocate an available input slot identifier.
   *
   * @param gamepad_mask Gamepad mask.
   * @return Allocated ID object, or null when unavailable.
   */
  template<std::size_t N>
  int alloc_id(std::bitset<N> &gamepad_mask) {
    for (int x = 0; x < gamepad_mask.size(); ++x) {
      if (!gamepad_mask[x]) {
        gamepad_mask[x] = true;
        return x;
      }
    }

    return -1;
  }

  /**
   * @brief Release ID resources.
   *
   * @param gamepad_mask Gamepad mask.
   * @param id Identifier for the controller, session, display, or resource.
   */
  template<std::size_t N>
  void free_id(std::bitset<N> &gamepad_mask, int id) {
    gamepad_mask[id] = false;
  }

  /**
   * @brief Packed identifier for a pressed key and its modifier flags.
   */
  typedef uint32_t key_press_id_t;

  /**
   * @brief Create a key-press identifier from the virtual-key code and flags.
   *
   * @param vk Virtual-key code from the client input packet.
   * @param flags Bit flags that modify the requested operation.
   * @return Constructed kpid object.
   */
  key_press_id_t make_kpid(uint16_t vk, uint8_t flags) {
    return (key_press_id_t) vk << 8 | flags;
  }

  /**
   * @brief Extract the virtual-key code from a packed key-press identifier.
   *
   * @param kpid Key-press identifier containing the virtual-key code and flags.
   * @return Virtual-key code stored in the high byte.
   */
  uint16_t vk_from_kpid(key_press_id_t kpid) {
    return kpid >> 8;
  }

  /**
   * @brief Extract the modifier flags from a packed key-press identifier.
   *
   * @param kpid Key-press identifier containing the virtual-key code and flags.
   * @return Modifier flags stored in the low byte.
   */
  uint8_t flags_from_kpid(key_press_id_t kpid) {
    return kpid & 0xFF;
  }

  /**
   * @brief Convert a little-endian netfloat to a native endianness float.
   * @param f Little-endian network float bytes.
   * @return Floating-point value decoded for the host CPU.
   */
  float from_netfloat(const netfloat f) {
    return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(f);
  }

  /**
   * @brief Convert a little-endian netfloat to a native float and clamp it to a range.
   * @param f Little-endian network float bytes.
   * @param min The minimium value for clamping.
   * @param max The maximum value for clamping.
   * @return Decoded floating-point value clamped between min and max.
   */
  float from_clamped_netfloat(netfloat f, float min, float max) {
    return std::clamp(from_netfloat(f), min, max);
  }

  static task_pool_util::TaskPool::task_id_t key_press_repeat_id {};
  static std::unordered_map<key_press_id_t, bool> key_press {};
  static std::array<std::uint8_t, 5> mouse_press {};

  static platf::input_t platf_input;
#ifdef SUNSHINE_TESTS
  /**
   * @brief Recorder that unit tests install in place of the platform keyboard.
   *
   * @return Mutable reference to the recorder, empty when no test installed one.
   */
  std::function<void(const testing::keyboard_event_t &)> &keyboard_sink() {
    static std::function<void(const testing::keyboard_event_t &)> sink;
    return sink;
  }
#endif
#ifdef SUNSHINE_TESTS
  /** @brief Optional observer replacing platform pen delivery in protocol tests. */
  std::function<void(const platf::touch_port_t &, const platf::pen_input_t &)> pen_sink;
#endif

  /**
   * @brief Deliver a normalized pen sample to its per-client backend.
   * @param input Stream owning the tablet.
   * @param viewport Normalized desktop viewport.
   * @param pen Normalized pen state.
   */
  void emit_pen(const std::shared_ptr<input_t> &input, const platf::touch_port_t &viewport, const platf::pen_input_t &pen);

  static std::bitset<platf::MAX_GAMEPADS> gamepadMask {};

  /**
   * @brief Release all platform resources associated with a virtual gamepad.
   *
   * @param platf_input Platf input.
   * @param id Identifier for the controller, session, display, or resource.
   */
  void free_gamepad(platf::input_t &platf_input, int id) {
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});
    platf::free_gamepad(platf_input, id);

    free_id(gamepadMask, id);
  }

  /**
   * @brief Per-client gamepad slot and feedback state.
   */
  struct gamepad_t {
    gamepad_t():
        gamepad_state {},
        back_timeout_id {},
        home_release_id {},
        id {-1},
        back_button_state {button_state_e::NONE} {
    }

    ~gamepad_t() {
      if (id >= 0) {
        if (task_pool.running()) {
          task_pool.push([id = this->id]() {
            ::input::free_gamepad(platf_input, id);
          });
        } else {
          ::input::free_gamepad(platf_input, id);
        }
      }
    }

    platf::gamepad_state_t gamepad_state;  ///< Gamepad state.

    thread_pool_util::ThreadPool::task_id_t back_timeout_id;  ///< Back timeout ID.

    thread_pool_util::ThreadPool::task_id_t home_release_id;  ///< Pending delayed Home button release, scheduled by back_timeout_id.

    int id;  ///< Global gamepad slot assigned to this client controller.

    // When emulating the HOME button, we may need to artificially release the back button.
    // Afterwards, the gamepad state on sunshine won't match the state on Moonlight.
    // To prevent Sunshine from sending erroneous input data to the active application,
    // Sunshine forces the button to be in a specific state until the gamepad state matches that of
    // Moonlight once more.
    button_state_e back_button_state;  ///< Back button state.
  };

  /**
   * @brief Tracks the side-specific client keys that contribute to one modifier flag.
   */
  struct modifier_state_t {
    /**
     * @brief Return whether any key for this modifier remains pressed.
     *
     * @return `true` when the generic, left, or right key is pressed.
     */
    [[nodiscard]] bool any_pressed() const {
      return generic_pressed || left_pressed || right_pressed;
    }

    bool generic_pressed = false;  ///< Whether the side-less modifier key is pressed.
    bool left_pressed = false;  ///< Whether the left modifier key is pressed.
    bool right_pressed = false;  ///< Whether the right modifier key is pressed.
  };

#ifdef __APPLE__
  /** @brief Per-session nativeTouchWorkaround contact and button state. */
  struct native_touch_state_t {
    std::map<std::uint32_t, CGPoint> contacts;  ///< Active contacts in stable ID order.
    std::chrono::steady_clock::time_point pair_sample_started {};  ///< Receipt time of the initial MOVE baseline.
    std::chrono::steady_clock::time_point pair_sample_latest {};  ///< Receipt time of the last MOVE included in the estimate.
    std::array<CGPoint, 2> previous_pair {};  ///< Pair positions after the last movement packet.
    platf::macos::touch_motion_t pending_pair_motion {};  ///< Signed changes accumulated in the current window.
    CGPoint origin {};  ///< Initial single-contact position.
    CGSize extent {};  ///< Capture display dimensions for the drag threshold.
    std::chrono::steady_clock::time_point down_time {};  ///< Start of the single-contact gesture.
    bool cursor_started = false;  ///< Whether single-finger movement has been sent.
    bool stationary = true;  ///< Whether the finger stayed within the tap dead zone.
    bool right_clicked = false;  ///< Whether long press consumed the single-contact gesture.
    CGPoint mouse_position {};  ///< Last single-contact position.
    CGPoint last_tap_position {};  ///< Position of the previous eligible left click.
    std::optional<std::chrono::steady_clock::time_point> last_tap_up;  ///< Previous eligible touch-up time.
    int left_click_count = 1;  ///< Matching click count for both left-button transitions.
    CGPoint right_position {};  ///< Position of a pending right-button release.
    bool left_down = false;  ///< Whether the workaround owns a pressed left button.
    bool multi_touch = false;  ///< Whether the current sequence has entered multi-touch.
    platf::macos::touch_gesture_mode gesture_mode = platf::macos::touch_gesture_mode::undecided;  ///< Locked two-finger output mode.
    bool magnifying = false;  ///< Whether Quartz received a magnify begin.
    thread_pool_util::ThreadPool::task_id_t cursor_task = nullptr;  ///< Deadline for the second finger to join before moving the cursor.
    thread_pool_util::ThreadPool::task_id_t tap_task = nullptr;  ///< Short left-click release deadline.
    thread_pool_util::ThreadPool::task_id_t hold_task = nullptr;  ///< Long-press deadline.
    thread_pool_util::ThreadPool::task_id_t right_task = nullptr;  ///< Right-button release deadline.
    thread_pool_util::ThreadPool::task_id_t sample_task = nullptr;  ///< Two-contact sample deadline.
  };
#endif

  /**
   * @brief Input emulation settings loaded from configuration.
   */
  struct input_t {
    /**
     * @brief Enumerates supported shortkey options.
     */
    enum shortkey_e {
      CTRL = 0x1,  ///< Control key
      ALT = 0x2,  ///< Alt key
      SHIFT = 0x4,  ///< Shift key
      SHORTCUT = CTRL | ALT | SHIFT  ///< Shortcut combination
    };

    /**
     * @brief Construct input state from the mailbox and platform backend.
     *
     * @param touch_port_event Event carrying the active touch port.
     * @param feedback_queue Queue used for controller feedback.
     */
    input_t(
      safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event,
      platf::feedback_queue_t feedback_queue
    ):
        shortcutFlags {},
        gamepads(MAX_GAMEPADS),
        client_context {platf::allocate_client_input_context(platf_input)},
        touch_port_event {std::move(touch_port_event)},
        feedback_queue {std::move(feedback_queue)},
        mouse_left_button_timeout {},
        touch_port {{0, 0, 0, 0, 0, 0}, 0, 0, 0.0f, 0.0f, 1.0f, 1.0f, 0, 0},
        accumulated_vscroll_delta {},
        accumulated_hscroll_delta {} {
    }

    // Keep track of alt+ctrl+shift key combo
    int shortcutFlags;  ///< Shortcut flags.

    modifier_state_t shift_keys;  ///< Client Shift keys contributing to the aggregate Shift flag.
    modifier_state_t control_keys;  ///< Client Control keys contributing to the aggregate Control flag.
    modifier_state_t alt_keys;  ///< Client Alt keys contributing to the aggregate Alt flag.

    std::vector<gamepad_t> gamepads;  ///< Virtual gamepad slots tracked for the stream.
    std::unique_ptr<platf::client_input_t> client_context;  ///< Client context.

    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event;  ///< Touch port event.
    platf::feedback_queue_t feedback_queue;  ///< Queue used to deliver controller feedback to the platform backend.

    std::list<std::vector<uint8_t>> input_queue;  ///< Validated input packets waiting for processing.
    std::mutex input_queue_lock;  ///< Input queue lock.
    pen_wire::roll_probe_t pen_roll_probe;  ///< Per-session hardware observations, protected by the queue lock.
    std::uint64_t input_generation = 0;  ///< Prevent old queued tasks from consuming resumed-session packets.
    bool input_stopped = false;  ///< Reject stale packets after disconnect until the session resumes.
    std::chrono::steady_clock::time_point next_invalid_pen_warning {};  ///< Rate limit malformed extension warnings.

    thread_pool_util::ThreadPool::task_id_t mouse_left_button_timeout;  ///< Mouse left button timeout.

#ifdef __APPLE__
    bool mac_magnify_active = false;  ///< Whether the client has an active Quartz magnification.
    bool mac_rotate_active = false;  ///< Whether the client has an active Quartz rotation.
    bool scroll_brake_left_down = false;  ///< Left press consumed to brake inertia, awaiting release.
    native_touch_state_t native_touch;  ///< Touchscreen compatibility state.
    bool relative_mouse_positioned = false;  ///< Whether the remote relative mouse has started on the capture display.
#endif
    input::touch_port_t touch_port;  ///< Touch coordinate bounds for the current stream.

    int32_t accumulated_vscroll_delta;  ///< Accumulated vscroll delta.
    int32_t accumulated_hscroll_delta;  ///< Accumulated hscroll delta.
  };

  void emit_pen(const std::shared_ptr<input_t> &input, const platf::touch_port_t &viewport, const platf::pen_input_t &pen) {
#ifdef SUNSHINE_TESTS
    if (pen_sink) {
      pen_sink(viewport, pen);
      return;
    }
#endif
    platf::pen_update(input->client_context.get(), viewport, pen);
  }

  /**
   * @brief Hash string-like session identifiers without allocating temporary strings.
   */
  struct transparent_string_hash_t {
    using is_transparent = void;  ///< Enable heterogeneous unordered-map lookup.

    /**
     * @brief Hash a string view.
     *
     * @param value Session identifier to hash.
     * @return Hash value for the supplied identifier.
     */
    std::size_t operator()(const std::string_view value) const noexcept {
      return std::hash<std::string_view> {}(value);
    }
  };

  using retained_input_map_t = std::unordered_map<
    std::string,
    std::shared_ptr<input_t>,
    transparent_string_hash_t,
    std::equal_to<>>;  ///< Retained inputs keyed by paired-client identity.

  /**
   * @brief Synchronized storage for retained input sessions.
   */
  struct retained_input_state_t {
    std::mutex mutex;  ///< Synchronizes retained session access across transport threads.
    retained_input_map_t inputs;  ///< Paused input sessions keyed by paired-client identity.
  };

  /**
   * @brief Access process-wide retained input session storage.
   *
   * @return Mutable retained input session state.
   */
  retained_input_state_t &retained_input_state() {
    static retained_input_state_t state;
    return state;
  }

  /**
   * @brief Execute lifecycle work on the input task thread when it is running.
   *
   * @tparam Function Callable type.
   * @param function Lifecycle operation to execute.
   */
  template<typename Function>
  void dispatch_input_task(Function &&function) {
    if (task_pool.running()) {
      task_pool.push(std::forward<Function>(function));
    } else {
      std::forward<Function>(function)();
    }
  }

  /**
   * @brief Destroy the virtual gamepads owned by retained input sessions.
   *
   * @param input Retained input session whose gamepads should be destroyed.
   */
  void destroy_gamepads(const std::shared_ptr<input_t> &input) {
    for (auto &gamepad : input->gamepads) {
      if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
      if (gamepad.home_release_id) {
        task_pool.cancel(gamepad.home_release_id);
        gamepad.home_release_id = nullptr;
      }
      if (gamepad.id >= 0) {
        ::input::free_gamepad(platf_input, gamepad.id);
        gamepad.id = -1;
      }
      gamepad.gamepad_state = {};
      gamepad.back_button_state = button_state_e::NONE;
    }
  }

  /**
   * @brief Destroy the virtual gamepads owned by retained input sessions.
   *
   * @param inputs Retained input sessions whose gamepads should be destroyed.
   */
  void destroy_gamepads(const retained_input_map_t &inputs) {
    for (const auto &[session_id, input] : inputs) {
      static_cast<void>(session_id);
      destroy_gamepads(input);
    }
  }

  /**
   * @brief Rebind retained input state to a resumed stream mailbox.
   *
   * @param input Retained input state.
   * @param mail Mailbox for the resumed stream connection.
   */
  void rebind_input(const std::shared_ptr<input_t> &input, const safe::mail_t &mail) {
    input->touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
#ifdef __APPLE__
    input->relative_mouse_positioned = false;
    input->touch_port = {};
#endif
    input->feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);

    for (int client_index = 0; client_index < input->gamepads.size(); ++client_index) {
      auto &gamepad = input->gamepads[client_index];
      if (gamepad.id < 0) {
        continue;
      }

      if (platf::rebind_gamepad(platf_input, {gamepad.id, static_cast<std::uint8_t>(client_index)}, input->feedback_queue) != 0) {
        free_id(gamepadMask, gamepad.id);
        gamepad.id = -1;
      }
    }
  }

  /**
   * @brief Apply shortcut based on VKEY
   * @param keyCode The VKEY code
   * @return 0 if no shortcut applied, > 0 if shortcut applied.
   */
  inline int apply_shortcut(short keyCode) {
    constexpr auto VK_F1 = 0x70;
    constexpr auto VK_F13 = 0x7C;

    BOOST_LOG(debug) << "Apply Shortcut: 0x"sv << util::hex((std::uint8_t) keyCode).to_string_view();

    if (keyCode >= VK_F1 && keyCode <= VK_F13) {
      mail::man->event<int>(mail::switch_display)->raise(keyCode - VK_F1);
      return 1;
    }

    switch (keyCode) {
      case 0x4E /* VKEY_N */:
        display_cursor = !display_cursor;
        config::save_show_cursor(display_cursor);
        mail::man->event<bool>(mail::switch_cursor)->raise(display_cursor);
        return 1;
    }

    return 0;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_REL_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin relative mouse move packet--"sv << std::endl
      << "deltaX ["sv << util::endian::big(packet->deltaX) << ']' << std::endl
      << "deltaY ["sv << util::endian::big(packet->deltaY) << ']' << std::endl
      << "--end relative mouse move packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_ABS_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin absolute mouse move packet--"sv << std::endl
      << "x      ["sv << util::endian::big(packet->x) << ']' << std::endl
      << "y      ["sv << util::endian::big(packet->y) << ']' << std::endl
      << "width  ["sv << util::endian::big(packet->width) << ']' << std::endl
      << "height ["sv << util::endian::big(packet->height) << ']' << std::endl
      << "--end absolute mouse move packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_MOUSE_BUTTON_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse button packet--"sv << std::endl
      << "action ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "button ["sv << util::hex(packet->button).to_string_view() << ']' << std::endl
      << "--end mouse button packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_SCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse scroll packet--"sv << std::endl
      << "scrollAmt1 ["sv << util::endian::big(packet->scrollAmt1) << ']' << std::endl
      << "--end mouse scroll packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PSS_HSCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse hscroll packet--"sv << std::endl
      << "scrollAmount ["sv << util::endian::big(packet->scrollAmount) << ']' << std::endl
      << "--end mouse hscroll packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_KEYBOARD_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin keyboard packet--"sv << std::endl
      << "keyAction ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "keyCode ["sv << util::hex(packet->keyCode).to_string_view() << ']' << std::endl
      << "modifiers ["sv << util::hex(packet->modifiers).to_string_view() << ']' << std::endl
      << "flags ["sv << util::hex(packet->flags).to_string_view() << ']' << std::endl
      << "--end keyboard packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_UNICODE_PACKET packet) {
    std::string text(packet->text, util::endian::big(packet->header.size) - sizeof(packet->header.magic));
    BOOST_LOG(debug)
      << "--begin unicode packet--"sv << std::endl
      << "text ["sv << text << ']' << std::endl
      << "--end unicode packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param packet Protocol packet being processed.
   */
  void print(PNV_MULTI_CONTROLLER_PACKET packet) {
    // Moonlight spams controller packet even when not necessary
    BOOST_LOG(verbose)
      << "--begin controller packet--"sv << std::endl
      << "controllerNumber ["sv << packet->controllerNumber << ']' << std::endl
      << "activeGamepadMask ["sv << util::hex(packet->activeGamepadMask).to_string_view() << ']' << std::endl
      << "buttonFlags ["sv << util::hex((uint32_t) packet->buttonFlags | (packet->buttonFlags2 << 16)).to_string_view() << ']' << std::endl
      << "leftTrigger ["sv << util::hex(packet->leftTrigger).to_string_view() << ']' << std::endl
      << "rightTrigger ["sv << util::hex(packet->rightTrigger).to_string_view() << ']' << std::endl
      << "leftStickX ["sv << packet->leftStickX << ']' << std::endl
      << "leftStickY ["sv << packet->leftStickY << ']' << std::endl
      << "rightStickX ["sv << packet->rightStickX << ']' << std::endl
      << "rightStickY ["sv << packet->rightStickY << ']' << std::endl
      << "--end controller packet--"sv;
  }

  /**
   * @brief Prints a touch packet.
   * @param packet The touch packet.
   */
  void print(PSS_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin touch packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "--end touch packet--"sv;
  }

  /**
   * @brief Prints a pen packet.
   * @param packet The pen packet.
   */
  void print(PSS_PEN_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin pen packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "toolType ["sv << util::hex(packet->toolType).to_string_view() << ']' << std::endl
      << "penButtons ["sv << util::hex(packet->penButtons).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "tilt ["sv << (uint32_t) packet->tilt << ']' << std::endl
      << "--end pen packet--"sv;
  }

  /**
   * @brief Prints a controller arrival packet.
   * @param packet The controller arrival packet.
   */
  void print(PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller arrival packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "type ["sv << util::hex(packet->type).to_string_view() << ']' << std::endl
      << "capabilities ["sv << util::hex(packet->capabilities).to_string_view() << ']' << std::endl
      << "supportedButtonFlags ["sv << util::hex(packet->supportedButtonFlags).to_string_view() << ']' << std::endl
      << "--end controller arrival packet--"sv;
  }

  /**
   * @brief Prints a controller touch packet.
   * @param packet The controller touch packet.
   */
  void print(PSS_CONTROLLER_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller touch packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressure ["sv << from_netfloat(packet->pressure) << ']' << std::endl
      << "--end controller touch packet--"sv;
  }

  /**
   * @brief Prints a controller motion packet.
   * @param packet The controller motion packet.
   */
  void print(PSS_CONTROLLER_MOTION_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller motion packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "motionType ["sv << util::hex(packet->motionType).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "z ["sv << from_netfloat(packet->z) << ']' << std::endl
      << "--end controller motion packet--"sv;
  }

  /**
   * @brief Prints a controller battery packet.
   * @param packet The controller battery packet.
   */
  void print(PSS_CONTROLLER_BATTERY_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller battery packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "batteryState ["sv << util::hex(packet->batteryState).to_string_view() << ']' << std::endl
      << "batteryPercentage ["sv << util::hex(packet->batteryPercentage).to_string_view() << ']' << std::endl
      << "--end controller battery packet--"sv;
  }

  /**
   * @brief Write a debug log representation of the input packet.
   */
  void print(void *payload) {
    auto header = (PNV_INPUT_HEADER) payload;

    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        print((PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        print((PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        print((PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        print((PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        print((PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        print((PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        print((PNV_UNICODE_PACKET) payload);
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        print((PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
      case SS_TOUCH_MAGIC:
        print((PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        print((PSS_PEN_PACKET) payload);
        break;
      case pen_wire::magic:
        {
          auto packet = reinterpret_cast<pen_wire::packet_t *>(payload);
          print(&packet->pen);
          BOOST_LOG(debug) << "barrelRoll: " << pen_wire::decode_roll(packet->barrelRoll);
          break;
        }
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        print((PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        print((PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        print((PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        print((PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
    }
  }

  /**
   * @brief Forward a client input packet directly to the platform backend.
   *
   * @param input Platform input backend that receives the event.
   * @param packet Protocol packet being processed.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    input->mouse_left_button_timeout = DISABLE_LEFT_BUTTON_DELAY;
#ifdef __APPLE__
    if (!input->relative_mouse_positioned) {
      if (auto port = input->touch_port_event->pop(0ms)) {
        input->touch_port = *port;
      }
      if (input->touch_port.native_display_id != 0) {
        input->relative_mouse_positioned = platf::center_relative_mouse(platf_input, input->touch_port.native_display_id);
      }
      return;
    }
#endif
    platf::move_mouse(platf_input, util::endian::big(packet->deltaX), util::endian::big(packet->deltaY));
  }

  /**
   * @brief Converts client coordinates on the specified surface into screen coordinates.
   * @param input The input context.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @return The host-relative coordinate pair if a touchport is available.
   */
  std::optional<std::pair<float, float>> client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size) {
    auto &touch_port_event = input->touch_port_event;
    auto &touch_port = input->touch_port;
    if (touch_port_event->peek()) {
      touch_port = *touch_port_event->pop();
    }
    if (!touch_port) {
      BOOST_LOG(verbose) << "Ignoring early absolute input without a touch port"sv;
      return std::nullopt;
    }

    auto scalarX = touch_port.width / size.first;
    auto scalarY = touch_port.height / size.second;

    float x = std::clamp(val.first, 0.0f, size.first) * scalarX;
    float y = std::clamp(val.second, 0.0f, size.second) * scalarY;

    auto offsetX = touch_port.client_offsetX;
    auto offsetY = touch_port.client_offsetY;

    x = std::clamp(x, offsetX, (size.first * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.second * scalarY) - offsetY);

    /*
    x and y here below have the coordinates of the surface of the streaming resolution,
    and are dependent on how that comes configured from the client (scalar_inv is calculated
    from the proportion of that and the device's **physical** size).
    */
    x = (x - offsetX) * touch_port.scalar_inv;
    y = (y - offsetY) * touch_port.scalar_inv;

    /*
    This final operation is a bit weird and has been brought about with lots of trial and error. A better
    way to do this may exist.

    Basically, this is what makes the touchscreen map to the logical virtual input coordinates properly.
    Since the virtual input dimensions are logical (because scaling breaks everything otherwise), using the previous
    x and y coordinates would be incorrect when screens are scaled, because the touch port is smaller (or larger)
    by a factor (that factor is touch_port.scalar_tpcoords), and that factor must be used to account for that difference
    when moving the cursor. Otherwise, it will move either slower or faster than your finger proportionally to
    scalar_tpcoords, and be offset *inversely* proportionally to scalar_tpcoords. So you must account for both differences
    by multiplying and dividing.
    */
    float final_x = (x + touch_port.offset_x * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    float final_y = (y + touch_port.offset_y * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    return std::pair {final_x, final_y};
  }

  /**
   * @brief Multiply a polar coordinate pair by a cartesian scaling factor.
   * @param r The radial coordinate.
   * @param angle The angular coordinate (radians).
   * @param scalar The scalar cartesian coordinate pair.
   * @return The scaled radial coordinate.
   */
  float multiply_polar_by_cartesian_scalar(float r, float angle, const std::pair<float, float> &scalar) {
    // Convert polar to cartesian coordinates
    float x = r * std::cos(angle);
    float y = r * std::sin(angle);

    // Scale the values
    x *= scalar.first;
    y *= scalar.second;

    // Convert the result back to a polar radial coordinate
    return std::sqrt(std::pow(x, 2) + std::pow(y, 2));
  }

  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar) {
    // If the rotation is unknown, we'll just scale both axes equally by using
    // a 45-degree angle for our scaling calculations
    float angle = rotation == LI_ROT_UNKNOWN ? (M_PI / 4) : (rotation * (M_PI / 180));

    // If we have a major but not a minor axis, treat the touch as circular
    float major = val.first;
    float minor = val.second != 0.0f ? val.second : val.first;

    // The minor axis is perpendicular to major axis so the angle must be rotated by 90 degrees
    return {multiply_polar_by_cartesian_scalar(major, angle, scalar), multiply_polar_by_cartesian_scalar(minor, angle + (M_PI / 2), scalar)};
  }

  /**
   * @brief Forward a client input packet directly to the platform backend.
   *
   * @param input Platform input backend that receives the event.
   * @param packet Protocol packet being processed.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (input->mouse_left_button_timeout == DISABLE_LEFT_BUTTON_DELAY) {
      input->mouse_left_button_timeout = ENABLE_LEFT_BUTTON_DELAY;
    }

    float x = util::endian::big(packet->x);
    float y = util::endian::big(packet->y);

    // Prevent divide by zero
    // Don't expect it to happen, but just in case
    if (!packet->width || !packet->height) {
      BOOST_LOG(warning) << "Moonlight passed invalid dimensions"sv;

      return;
    }

    auto width = (float) util::endian::big(packet->width);
    auto height = (float) util::endian::big(packet->height);

    auto tpcoords = client_to_touchport(input, {x, y}, {width, height});
    if (!tpcoords) {
      return;
    }

    auto &touch_port = input->touch_port;

    int touch_port_dim_x;
    int touch_port_dim_y;
    if (touch_port.env_logical_width != 0 && touch_port.env_logical_height != 0) {
      touch_port_dim_x = touch_port.env_logical_width;
      touch_port_dim_y = touch_port.env_logical_height;
    } else {
      touch_port_dim_x = touch_port.env_width;
      touch_port_dim_y = touch_port.env_height;
    }

    platf::touch_port_t abs_port {
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port_dim_x,
      touch_port_dim_y,
      touch_port.logical_width,
      touch_port.logical_height,
    };

    platf::abs_mouse(platf_input, abs_port, tpcoords->first, tpcoords->second);
  }

  /**
   * @brief Called to pass a mouse button message to the platform backend.
   *
   * @param input The input context pointer.
   * @param packet The mouse button packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
    auto button = util::endian::big(packet->button);
#ifdef __APPLE__
    if (button == BUTTON_LEFT) {
      if (config::input.macos_smooth_scrolling_diagnostics == "trace" ||
          (std::getenv("LUMEN_SCROLL_DIAGNOSTICS") && std::string_view {std::getenv("LUMEN_SCROLL_DIAGNOSTICS")} == "trace")) {
        BOOST_LOG(info) << "macOS scroll trace: mouse packet"
                        << " button=" << static_cast<int>(button) << " release=" << release
                        << " pressed=" << static_cast<bool>(mouse_press[button])
                        << " suppressed=" << input->scroll_brake_left_down;
      }
      if (input->scroll_brake_left_down) {
        if (release) {
          input->scroll_brake_left_down = false;
        }
        return;
      }
      // A release for an already-up button must not consume the brake.
      if (mouse_press[button] == release && platf::macos::brake_scroll_momentum() && !release) {
        input->scroll_brake_left_down = true;
        return;
      }
    }
#endif
    if (button > 0 && button < mouse_press.size()) {
      if (mouse_press[button] != release) {
        // button state is already what we want
        return;
      }

      mouse_press[button] = !release;
    }
    /**
     * When Moonlight sends mouse input through absolute coordinates,
     * it's possible that BUTTON_RIGHT is pressed down immediately after releasing BUTTON_LEFT.
     * As a result, Sunshine will left-click on hyperlinks in the browser before right-clicking
     *
     * This can be solved by delaying BUTTON_LEFT, however, any delay on input is undesirable during gaming
     * As a compromise, Sunshine will only put delays on BUTTON_LEFT when
     * absolute mouse coordinates have been sent.
     *
     * Try to make sure BUTTON_RIGHT gets called before BUTTON_LEFT is released.
     *
     * input->mouse_left_button_timeout can only be nullptr
     * when the last mouse coordinates were absolute
     */
    if (button == BUTTON_LEFT && release && !input->mouse_left_button_timeout) {
      auto f = [=]() {
        auto left_released = mouse_press[BUTTON_LEFT];
        if (left_released) {
          // Already released left button
          return;
        }
        platf::button_mouse(platf_input, BUTTON_LEFT, release);

        mouse_press[BUTTON_LEFT] = false;
        input->mouse_left_button_timeout = nullptr;
      };

      input->mouse_left_button_timeout = task_pool.pushDelayed(std::move(f), 10ms).task_id;

      return;
    }
    if (
      button == BUTTON_RIGHT && !release &&
      input->mouse_left_button_timeout > DISABLE_LEFT_BUTTON_DELAY
    ) {
      platf::button_mouse(platf_input, BUTTON_RIGHT, false);
      platf::button_mouse(platf_input, BUTTON_RIGHT, true);

      mouse_press[BUTTON_RIGHT] = false;

      return;
    }

    platf::button_mouse(platf_input, button, release);
  }

  /**
   * @brief Apply configured keybinding remaps to a platform keycode.
   *
   * @param keycode Platform keycode being translated or emitted.
   * @return Remapped keycode when configured, otherwise the original keycode.
   */
  short map_keycode(short keycode) {
    auto it = config::input.keybindings.find(keycode);
    if (it != std::end(config::input.keybindings)) {
      return it->second;
    }

    return keycode;
  }

  /**
   * @brief Update the side-specific state for a client modifier key.
   *
   * @param input Input context tracking the modifier keys.
   * @param key_code Moonlight keyboard packet key code.
   * @param release Whether the key event is a release.
   */
  void update_modifier_state(input_t &input, short key_code, bool release) {
    const bool pressed = !release;
    switch (key_code) {
      case VKEY_SHIFT:
        input.shift_keys.generic_pressed = pressed;
        break;
      case VKEY_LSHIFT:
        input.shift_keys.left_pressed = pressed;
        break;
      case VKEY_RSHIFT:
        input.shift_keys.right_pressed = pressed;
        break;
      case VKEY_CONTROL:
        input.control_keys.generic_pressed = pressed;
        break;
      case VKEY_LCONTROL:
        input.control_keys.left_pressed = pressed;
        break;
      case VKEY_RCONTROL:
        input.control_keys.right_pressed = pressed;
        break;
      case VKEY_MENU:
        input.alt_keys.generic_pressed = pressed;
        break;
      case VKEY_LMENU:
        input.alt_keys.left_pressed = pressed;
        break;
      case VKEY_RMENU:
        input.alt_keys.right_pressed = pressed;
        break;
      default:
        break;
    }
  }

  /**
   * @brief Update flags for keyboard shortcut combo's
   *
   * @param input Input context tracking which side-specific modifier keys are held.
   * @param keyCode Moonlight keyboard packet key code.
   * @param release Whether the key or button event is a release.
   */
  inline void update_shortcutFlags(input_t &input, short keyCode, bool release) {
    int *flags = &input.shortcutFlags;
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
        if (release) {
          if (!input.shift_keys.any_pressed()) {
            *flags &= ~input_t::SHIFT;
          }
        } else {
          *flags |= input_t::SHIFT;
        }
        break;
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
        if (release) {
          if (!input.control_keys.any_pressed()) {
            *flags &= ~input_t::CTRL;
          }
        } else {
          *flags |= input_t::CTRL;
        }
        break;
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        // Left, right, and side-less Alt all set the same aggregate ALT bit, so releasing
        // one of them must not clear it while another is still held (e.g. Right Alt mapped
        // to Meta via key_rightalt_to_key_win, released while Left Alt remains down).
        if (release) {
          if (!input.alt_keys.any_pressed()) {
            *flags &= ~input_t::ALT;
          }
        } else {
          *flags |= input_t::ALT;
        }
        break;
    }
  }

  /**
   * @brief Check whether modifier.
   *
   * @param keyCode Moonlight keyboard packet key code.
   * @return True when the key code is a keyboard modifier.
   */
  bool is_modifier(uint16_t keyCode) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        return true;
      default:
        return false;
    }
  }

  /**
   * @brief Deliver one keyboard event to the platform backend.
   *
   * Test builds can divert the event to a recorder so unit tests never type into the host.
   *
   * @param key_code Platform keycode to emit.
   * @param release Whether the key event is a release.
   * @param flags Bit flags that modify the requested operation.
   */
  void emit_keyboard_update(uint16_t key_code, bool release, uint8_t flags) {
#ifdef SUNSHINE_TESTS
    if (keyboard_sink()) {
      keyboard_sink()(testing::keyboard_event_t {key_code, release, flags});
      return;
    }
#endif
    platf::keyboard_update(platf_input, key_code, release, flags);
  }

  /**
   * @brief Send key and modifiers.
   *
   * @param key_code Moonlight keyboard packet key code.
   * @param release Whether the key or button event is a release.
   * @param flags Bit flags that modify the requested operation.
   * @param synthetic_modifiers Synthetic modifiers.
   */
  void send_key_and_modifiers(uint16_t key_code, bool release, uint8_t flags, uint8_t synthetic_modifiers) {
    if (!release) {
      // Press any synthetic modifiers required for this key
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        emit_keyboard_update(VKEY_SHIFT, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        emit_keyboard_update(VKEY_CONTROL, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        emit_keyboard_update(VKEY_MENU, false, flags);
      }
    }

    emit_keyboard_update(map_keycode(key_code), release, flags);

    if (!release) {
      // Raise any synthetic modifier keys we pressed
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        emit_keyboard_update(VKEY_SHIFT, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        emit_keyboard_update(VKEY_CONTROL, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        emit_keyboard_update(VKEY_MENU, true, flags);
      }
    }
  }

  /**
   * @brief Re-emit a held key until its repeat task is cancelled.
   *
   * @param key_code Moonlight keyboard packet key code.
   * @param flags Bit flags that modify the requested operation.
   * @param synthetic_modifiers Synthetic modifiers.
   */
  void repeat_key(uint16_t key_code, uint8_t flags, uint8_t synthetic_modifiers) {
    // If key no longer pressed, stop repeating
    if (!key_press[make_kpid(key_code, flags)]) {
      key_press_repeat_id = nullptr;
      return;
    }

    send_key_and_modifiers(key_code, false, flags, synthetic_modifiers);

    key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_period, key_code, flags, synthetic_modifiers).task_id;
  }

  /**
   * @brief Forward a client input packet directly to the platform backend.
   *
   * @param input Platform input backend that receives the event.
   * @param packet Protocol packet being processed.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == KEY_UP_EVENT_MAGIC;
    auto keyCode = packet->keyCode & 0x00FF;

    update_modifier_state(*input, keyCode, release);

    // Right-alt maps to meta, so it must not also register as ALT
    int modifiers = packet->modifiers;
    if (config::input.key_rightalt_to_key_win && input->alt_keys.right_pressed && !input->alt_keys.left_pressed) {
      modifiers &= ~MODIFIER_ALT;
    }

    // Set synthetic modifier flags if the keyboard packet is requesting modifier
    // keys that are not current pressed.
    uint8_t synthetic_modifiers = 0;
    if (!release && !is_modifier(keyCode)) {
      if (!(input->shortcutFlags & input_t::SHIFT) && (modifiers & MODIFIER_SHIFT)) {
        synthetic_modifiers |= MODIFIER_SHIFT;
      }
      if (!(input->shortcutFlags & input_t::CTRL) && (modifiers & MODIFIER_CTRL)) {
        synthetic_modifiers |= MODIFIER_CTRL;
      }
      if (!(input->shortcutFlags & input_t::ALT) && (modifiers & MODIFIER_ALT)) {
        synthetic_modifiers |= MODIFIER_ALT;
      }
    }

    auto &pressed = key_press[make_kpid(keyCode, packet->flags)];
    if (!pressed) {
      if (!release) {
        // A new key has been pressed down, we need to check for key combo's
        // If a key-combo has been pressed down, don't pass it through
        if (input->shortcutFlags == input_t::SHORTCUT && apply_shortcut(keyCode) > 0) {
          return;
        }

        if (key_press_repeat_id) {
          task_pool.cancel(key_press_repeat_id);
        }

        if (config::input.key_repeat_delay.count() > 0) {
          key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_delay, keyCode, packet->flags, synthetic_modifiers).task_id;
        }
      } else {
        // Already released
        return;
      }
    } else if (!release) {
      // Already pressed down key
      return;
    }

    pressed = !release;

    send_key_and_modifiers(keyCode, release, packet->flags, synthetic_modifiers);

    // Track the modifier state the client is holding, not the remapped host key.
    // This is compared against packet->modifiers above, which is client-side, so a
    // keybinding that moves Alt off VKEY_*MENU must not clear the ALT bit here.
    update_shortcutFlags(*input, keyCode, release);
  }

  /**
   * @brief Called to pass a vertical scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling || config::input.macos_smooth_scrolling) {
      platf::scroll(platf_input, util::endian::big(packet->scrollAmt1));
    } else {
      input->accumulated_vscroll_delta += util::endian::big(packet->scrollAmt1);
      auto full_ticks = input->accumulated_vscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::scroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_vscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  /**
   * @brief Called to pass a horizontal scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling || config::input.macos_smooth_scrolling) {
      platf::hscroll(platf_input, util::endian::big(packet->scrollAmount));
    } else {
      input->accumulated_hscroll_delta += util::endian::big(packet->scrollAmount);
      auto full_ticks = input->accumulated_hscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::hscroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_hscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  /**
   * @brief Forward a client input packet directly to the platform backend.
   *
   * @param packet Protocol packet being processed.
   */
  void passthrough(const NV_UNICODE_PACKET *packet) {
    if (!config::input.keyboard) {
      return;
    }

    int size = util::endian::big(packet->header.size) - sizeof(packet->header.magic);
    platf::unicode(platf_input, packet->text, size);
  }

  /**
   * @brief Allocate a virtual gamepad for a client-relative controller slot.
   *
   * @param input Stream input state.
   * @param client_index Client-relative controller index.
   * @param arrival Client-reported controller metadata.
   * @return Assigned global gamepad slot, or -1 when allocation fails.
   */
  int alloc_gamepad(std::shared_ptr<input_t> &input, int client_index, const platf::gamepad_arrival_t &arrival) {
    if (client_index < 0 || client_index >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << client_index << ']';
      return -1;
    }

    auto &gamepad = input->gamepads[client_index];
    if (gamepad.id >= 0) {
      BOOST_LOG(warning) << "ControllerNumber already allocated ["sv << client_index << ']';
      return gamepad.id;
    }

    const auto id = alloc_id(gamepadMask);
    if (id < 0) {
      return -1;
    }

    if (platf::alloc_gamepad(platf_input, {id, static_cast<std::uint8_t>(client_index)}, arrival, input->feedback_queue)) {
      free_id(gamepadMask, id);
      return -1;
    }

    gamepad.id = id;
    return id;
  }

  /**
   * @brief Called to pass a controller arrival message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller arrival packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    platf::gamepad_arrival_t arrival {
      packet->type,
      util::endian::little(packet->capabilities),
      util::endian::little(packet->supportedButtonFlags),
    };
    static_cast<void>(alloc_gamepad(input, packet->controllerNumber, arrival));
  }

  /**
   * @brief Normalizes coordinates to monitor-local logical touch dimensions.
   * @param touch_port The current touch port metadata.
   * @param coords The in/out coordinate pair to normalize.
   * @return The monitor-local touch port, or std::nullopt if dimensions are invalid.
   */
  std::optional<platf::touch_port_t> monitor_touch_port(const input::touch_port_t &touch_port, std::pair<float, float> &coords) {
    const float monitor_logical_w = (touch_port.width * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    const float monitor_logical_h = (touch_port.height * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    if (monitor_logical_w <= 0.0f || monitor_logical_h <= 0.0f) {
      BOOST_LOG(warning) << "Ignoring touch/pen input due to invalid logical touch dimensions"sv;
      return std::nullopt;
    }

    coords.first = (coords.first - touch_port.offset_x) / monitor_logical_w;
    coords.second = (coords.second - touch_port.offset_y) / monitor_logical_h;

    return platf::touch_port_t {
      touch_port.offset_x,
      touch_port.offset_y,
      static_cast<int>(monitor_logical_w),
      static_cast<int>(monitor_logical_h),
      static_cast<int>(monitor_logical_w),
      static_cast<int>(monitor_logical_h),
      touch_port.native_display_id,
    };
  }

  /**
   * @brief Shared normalized data prepared for a touch or pen event.
   */
  struct absolute_pointer_data_t {
    platf::touch_port_t touch_port;  ///< Monitor-local touch port.
    std::pair<float, float> coords;  ///< Normalized monitor-local coordinates.
    std::uint16_t rotation;  ///< Normalized rotation in degrees.
    std::pair<float, float> contact_area;  ///< Scaled major and minor contact axes.
  };

  /**
   * @brief Normalize the fields shared by touch and pen packets.
   *
   * @tparam Packet Pointer type for a Moonlight touch or pen packet.
   * @param input Input context that supplies the current touch-port metadata.
   * @param packet Touch or pen packet to normalize.
   * @return Normalized pointer data, or `std::nullopt` when input is disabled or dimensions are invalid.
   */
  template<typename Packet>
  std::optional<absolute_pointer_data_t> prepare_absolute_pointer_data(std::shared_ptr<input_t> &input, Packet packet) {
    if (!config::input.mouse) {
      return std::nullopt;
    }

    auto coords = client_to_touchport(
      input,
      {from_clamped_netfloat(packet->x, 0.0f, 1.0f) * 65535.f, from_clamped_netfloat(packet->y, 0.0f, 1.0f) * 65535.f},
      {65535.f, 65535.f}
    );
    if (!coords) {
      return std::nullopt;
    }

    auto touch_port = monitor_touch_port(input->touch_port, *coords);
    if (!touch_port) {
      return std::nullopt;
    }

    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    const auto contact_area = scale_client_contact_area(
      {from_clamped_netfloat(packet->contactAreaMajor, 0.0f, 1.0f) * 65535.f,
       from_clamped_netfloat(packet->contactAreaMinor, 0.0f, 1.0f) * 65535.f},
      rotation,
      {touch_port->width / 65535.f, touch_port->height / 65535.f}
    );

    return absolute_pointer_data_t {*touch_port, *coords, rotation, contact_area};
  }

#ifdef __APPLE__
  /**
   * @brief Post an absolute mouse event at a touchscreen coordinate.
   * @param type Quartz mouse event type.
   * @param position Global cursor position.
   * @param button Mouse button associated with the event.
   * @param click_count Number of clicks in the current sequence.
   */
  void post_native_touch_mouse(CGEventType type, CGPoint position, CGMouseButton button = kCGMouseButtonLeft, int click_count = 1) {
    if (auto event = CGEventCreateMouseEvent(nullptr, type, position, button)) {
      CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
      if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseDragged || type == kCGEventMouseMoved) {
        CGWarpMouseCursorPosition(position);
      }
    }
  }

  /**
   * @brief End a pending long press and release the touch-owned left button.
   * @param state Touchscreen compatibility state.
   */
  void release_native_touch_left(native_touch_state_t &state) {
    if (state.cursor_task) {
      task_pool.cancel(state.cursor_task);
      state.cursor_task = nullptr;
    }
    if (state.hold_task) {
      task_pool.cancel(state.hold_task);
      state.hold_task = nullptr;
    }
    if (state.tap_task) {
      task_pool.cancel(state.tap_task);
      state.tap_task = nullptr;
    }
    if (state.left_down) {
      post_native_touch_mouse(kCGEventLeftMouseUp, state.mouse_position, kCGMouseButtonLeft, state.left_click_count);
      state.left_click_count = 1;
      state.left_down = false;
    }
  }

  /**
   * @brief Release a touch-owned right button and cancel its delayed release.
   * @param state Touchscreen compatibility state.
   */
  void release_native_touch_right(native_touch_state_t &state) {
    if (state.right_task) {
      task_pool.cancel(state.right_task);
      state.right_task = nullptr;
      post_native_touch_mouse(kCGEventRightMouseUp, state.right_position, kCGMouseButtonRight);
    }
  }

  /**
   * @brief Forward a macOS trackpad gesture to Quartz.
   * @param input Stream input context.
   * @param packet Gesture packet.
   * @param rotate Whether the packet carries rotation.
   */
  void passthrough(std::shared_ptr<input_t> &input, const mac_trackpad_wire::packet_t &packet, bool rotate) {
    if (!config::input.mouse) {
      return;
    }
    using platf::macos::gesture_phase;
    const auto phase = static_cast<gesture_phase>(packet.phase);
    auto &active = rotate ? input->mac_rotate_active : input->mac_magnify_active;
    const auto post = rotate ? platf::macos::touch_rotate : platf::macos::touch_magnify;
    switch (phase) {
      case gesture_phase::began:
        if (!active) {
          platf::macos::cancel_touch_scroll();
          active = post(phase, 0);
        }
        break;
      case gesture_phase::changed:
        if (const auto amount = from_netfloat(packet.amount);
            active && std::isfinite(amount) && (rotate ? std::abs(amount) <= 180 : amount > -1)) {
          post(phase, amount);
        }
        break;
      case gesture_phase::ended:
      case gesture_phase::cancelled:
        if (active) {
          post(phase, 0);
          active = false;
        }
        break;
    }
  }

  /**
   * @brief End a magnify sequence if one is active.
   * @param state Touchscreen compatibility state.
   * @param cancelled Whether to cancel the gesture.
   */
  void finish_native_touch_magnify(native_touch_state_t &state, bool cancelled) {
    if (state.magnifying) {
      using platf::macos::gesture_phase;
      platf::macos::touch_magnify(cancelled ? gesture_phase::cancelled : gesture_phase::ended, 0);
      state.magnifying = false;
    }
  }

  /**
   * @brief Consume the latest positions of both contacts together.
   * @param state Touchscreen compatibility state.
   */
  void sample_native_touch_pair(native_touch_state_t &state) {
    const auto displacement = std::exchange(state.pending_pair_motion, {});
    const auto sample_seconds = std::chrono::duration<double>(state.pair_sample_latest - state.pair_sample_started).count();
    if (state.gesture_mode == platf::macos::touch_gesture_mode::undecided && sample_seconds <= 0) {
      return;
    }
    const auto motion = platf::macos::classify_touch_motion(displacement, sample_seconds, state.gesture_mode);
    if (motion.magnification != 0) {
      using platf::macos::gesture_phase;
      if (!state.magnifying) {
        platf::macos::cancel_touch_scroll();
        state.magnifying = platf::macos::touch_magnify(gesture_phase::began, 0);
      }
      if (state.magnifying) {
        platf::macos::touch_magnify(gesture_phase::changed, std::expm1(motion.magnification / 300));
      }
    } else if (motion.x != 0 || motion.y != 0) {
      platf::macos::touch_scroll(motion.x, motion.y);
    }
  }

  /**
   * @brief Apply nativeTouchWorkaround to a touchscreen contact packet.
   * @param input Stream input context.
   * @param packet Contact packet.
   */
  void native_touch_workaround(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet) {
    auto &state = input->native_touch;
    const auto id = util::endian::little(packet->pointerId);
    const auto type = packet->eventType;
    const bool release = type == LI_TOUCH_EVENT_UP || type == LI_TOUCH_EVENT_CANCEL || type == LI_TOUCH_EVENT_CANCEL_ALL;
    if (type != LI_TOUCH_EVENT_DOWN && type != LI_TOUCH_EVENT_MOVE && !release) {
      return;
    }
    if (type != LI_TOUCH_EVENT_DOWN && type != LI_TOUCH_EVENT_CANCEL_ALL && !state.contacts.contains(id)) {
      return;
    }

    if (type == LI_TOUCH_EVENT_DOWN && (state.contacts.size() >= 16 || state.contacts.contains(id))) {
      return;
    }

    if (release || type == LI_TOUCH_EVENT_DOWN) {
      if (state.sample_task) {
        task_pool.cancel(state.sample_task);
        state.sample_task = nullptr;
        if (state.gesture_mode != platf::macos::touch_gesture_mode::undecided &&
            type != LI_TOUCH_EVENT_CANCEL && type != LI_TOUCH_EVENT_CANCEL_ALL) {
          sample_native_touch_pair(state);
        } else {
          state.pending_pair_motion = {};
        }
      }
    }

    constexpr auto drag_delay = 100ms;
    const auto weak_input = std::weak_ptr {input};
    std::optional<CGPoint> position;
    CGSize extent {};
    if (type == LI_TOUCH_EVENT_DOWN || type == LI_TOUCH_EVENT_MOVE || type == LI_TOUCH_EVENT_UP) {
      if (auto data = prepare_absolute_pointer_data(input, packet)) {
        const auto &port = data->touch_port;
        const auto bounds = port.native_display_id
          ? CGDisplayBounds(port.native_display_id)
          : CGRectMake(port.offset_x, port.offset_y, port.width, port.height);
        const auto [x, y] = data->coords;
        if (!CGRectIsEmpty(bounds) && std::isfinite(x) && std::isfinite(y)) {
          extent = bounds.size;
          position = CGPointMake(bounds.origin.x + std::clamp(x, 0.f, 1.f) * (bounds.size.width - 1),
                                 bounds.origin.y + std::clamp(y, 0.f, 1.f) * (bounds.size.height - 1));
        }
      }
    }

    if (release) {
      const auto up_time = std::chrono::steady_clock::now();
      const bool tap = type == LI_TOUCH_EVENT_UP && position && !state.multi_touch &&
                       !state.left_down && !state.right_clicked && state.stationary &&
                       platf::macos::touch_is_stationary(state.origin, *position) &&
                       up_time - state.down_time < drag_delay;
      if (position && !state.multi_touch && state.left_down) {
        state.mouse_position = *position;
      }
      if (!state.multi_touch) {
        release_native_touch_left(state);
      }
      if (tap) {
        const bool double_tap = state.last_tap_up && platf::macos::touch_is_double_tap(
          state.last_tap_position, *position, extent, state.down_time - *state.last_tap_up
        );
        state.left_click_count = double_tap ? 2 : 1;
        state.mouse_position = double_tap ? state.last_tap_position : *position;
        state.last_tap_position = state.mouse_position;
        state.last_tap_up = double_tap ? std::nullopt : std::optional {up_time};
        post_native_touch_mouse(kCGEventLeftMouseDown, state.mouse_position, kCGMouseButtonLeft, state.left_click_count);
        state.left_down = true;
        state.tap_task = task_pool.pushDelayed([weak_input]() {
          if (auto input = weak_input.lock()) {
            auto &state = input->native_touch;
            state.tap_task = nullptr;
            release_native_touch_left(state);
          }
        }, 30ms).task_id;
      } else {
        state.last_tap_up.reset();
      }
      if (type == LI_TOUCH_EVENT_CANCEL_ALL) {
        state.contacts.clear();
      } else {
        state.contacts.erase(id);
      }
      if (state.contacts.empty()) {
        finish_native_touch_magnify(state, type != LI_TOUCH_EVENT_UP);
        state.gesture_mode = platf::macos::touch_gesture_mode::undecided;
        state.multi_touch = false;
      } else if (state.contacts.size() == 2) {
        platf::macos::cancel_touch_scroll();
        state.previous_pair = {state.contacts.begin()->second, std::next(state.contacts.begin())->second};
      }
      return;
    }
    if (!position) {
      return;
    }

    if (type == LI_TOUCH_EVENT_DOWN) {
      state.contacts.emplace(id, *position);
      if (state.contacts.size() > 1) {
        if (!state.multi_touch) {
          release_native_touch_left(state);
          release_native_touch_right(state);
          state.multi_touch = true;
        }
        if (state.contacts.size() == 2) {
          platf::macos::cancel_touch_scroll();
          state.previous_pair = {state.contacts.begin()->second, std::next(state.contacts.begin())->second};
        }
        return;
      }
      release_native_touch_left(state);
      state.origin = state.mouse_position = *position;
      state.extent = extent;
      state.down_time = std::chrono::steady_clock::now();
      state.cursor_started = false;
      state.stationary = true;
      state.right_clicked = false;
      state.cursor_task = task_pool.pushDelayed([weak_input]() {
        if (auto input = weak_input.lock()) {
          auto &state = input->native_touch;
          state.cursor_task = nullptr;
          if (!state.multi_touch && !platf::macos::touch_is_stationary(state.origin, state.mouse_position)) {
            post_native_touch_mouse(kCGEventMouseMoved, state.mouse_position);
            state.cursor_started = true;
          }
        }
      }, platf::macos::touch_cursor_wait).task_id;
      state.hold_task = task_pool.pushDelayed([weak_input]() {
        if (auto input = weak_input.lock()) {
          auto &state = input->native_touch;
          state.hold_task = nullptr;
          if (!state.stationary || state.multi_touch) {
            return;
          }
          release_native_touch_left(state);
          state.right_clicked = true;
          state.right_position = state.mouse_position;
          post_native_touch_mouse(kCGEventMouseMoved, state.right_position);
          post_native_touch_mouse(kCGEventRightMouseDown, state.right_position, kCGMouseButtonRight);
          state.right_task = task_pool.pushDelayed([weak_input]() {
            if (auto input = weak_input.lock()) {
              auto &state = input->native_touch;
              state.right_task = nullptr;
              post_native_touch_mouse(kCGEventRightMouseUp, state.right_position, kCGMouseButtonRight);
            }
          }, 10ms).task_id;
        }
      }, 650ms).task_id;
      return;
    }

    state.contacts.at(id) = *position;
    if (state.contacts.size() == 2) {
      const auto a = state.contacts.begin()->second;
      const auto b = std::next(state.contacts.begin())->second;
      const auto received = std::chrono::steady_clock::now();
      const bool undecided = state.gesture_mode == platf::macos::touch_gesture_mode::undecided;
      if (!undecided || state.sample_task) {
        const auto delta = platf::macos::measure_touch_motion(state.previous_pair[0], state.previous_pair[1], a, b);
        state.pending_pair_motion.x += delta.x;
        state.pending_pair_motion.y += delta.y;
        state.pending_pair_motion.magnification += delta.magnification;
      }
      state.previous_pair = {a, b};
      state.pair_sample_latest = received;

      if (!state.sample_task) {
        state.pair_sample_started = received;
        const auto delay = undecided
          ? platf::macos::touch_gesture_decision_window
          : platf::macos::touch_gesture_output_interval;
        state.sample_task = task_pool.pushDelayed([weak_input]() {
          if (auto input = weak_input.lock()) {
            auto &state = input->native_touch;
            state.sample_task = nullptr;
            sample_native_touch_pair(state);
          }
        }, delay).task_id;
      }
    } else if (!state.multi_touch && !state.right_clicked) {
      state.stationary &= platf::macos::touch_is_stationary(state.origin, *position);
      if (platf::macos::touch_exceeds_drag_threshold(state.origin, *position, state.extent)) {
        if (state.hold_task) {
          task_pool.cancel(state.hold_task);
          state.hold_task = nullptr;
        }
        if (!state.left_down && std::chrono::steady_clock::now() - state.down_time > drag_delay) {
          post_native_touch_mouse(kCGEventLeftMouseDown, state.mouse_position);
          state.left_down = true;
        }
      }
      state.mouse_position = *position;
      if (!state.cursor_task && (state.cursor_started || !platf::macos::touch_is_stationary(state.origin, *position))) {
        post_native_touch_mouse(state.left_down ? kCGEventLeftMouseDragged : kCGEventMouseMoved, *position);
        state.cursor_started = true;
      }
    }
  }
#endif

  /**
   * @brief Called to pass a touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet) {
#ifdef __APPLE__
    if (config::input.mouse && config::input.native_pen_touch) {
      native_touch_workaround(input, packet);
    }
#else
    const auto pointer_data = prepare_absolute_pointer_data(input, packet);
    if (!pointer_data) {
      return;
    }

    platf::touch_input_t touch {
      packet->eventType,
      pointer_data->rotation,
      util::endian::little(packet->pointerId),
      pointer_data->coords.first,
      pointer_data->coords.second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      pointer_data->contact_area.first,
      pointer_data->contact_area.second,
    };

    platf::touch_update(input->client_context.get(), pointer_data->touch_port, touch);
#endif
  }

  /**
   * @brief Called to pass a pen message to the platform backend.
   * @param input The input context pointer.
   * @param packet The pen packet.
   * @param barrel_roll Resolved independent barrel roll, or unknown for legacy azimuth fallback.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_PEN_PACKET packet, std::uint16_t barrel_roll = pen_wire::unknown) {
#ifdef __APPLE__
    // Quartz retains the last valid point for events without coordinates.
    if (packet->eventType == LI_TOUCH_EVENT_CANCEL_ALL || packet->eventType == LI_TOUCH_EVENT_CANCEL || packet->eventType == LI_TOUCH_EVENT_HOVER_LEAVE || packet->eventType == LI_TOUCH_EVENT_BUTTON_ONLY) {
      platf::pen_input_t pen {};
      pen.eventType = packet->eventType;
      pen.toolType = packet->toolType;
      pen.penButtons = packet->penButtons;
      emit_pen(input, {}, pen);
      return;
    }
#endif
    const auto pointer_data = prepare_absolute_pointer_data(input, packet);
    if (!pointer_data) {
      return;
    }

    platf::pen_input_t pen {
      packet->eventType,
      packet->toolType,
      packet->penButtons,
      packet->tilt,
      pointer_data->rotation,
      pointer_data->coords.first,
      pointer_data->coords.second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      pointer_data->contact_area.first,
      pointer_data->contact_area.second,
    };

    pen.barrelRoll = barrel_roll == pen_wire::unknown ? pen.rotation : barrel_roll;
    emit_pen(input, pointer_data->touch_port, pen);
  }

  /**
   * @brief Called to pass a controller touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_TOUCH_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_touch_t touch {
      {gamepad.id, packet->controllerNumber},
      packet->eventType,
      util::endian::little(packet->pointerId),
      from_clamped_netfloat(packet->x, 0.0f, 1.0f),
      from_clamped_netfloat(packet->y, 0.0f, 1.0f),
      from_clamped_netfloat(packet->pressure, 0.0f, 1.0f),
    };

    platf::gamepad_touch(platf_input, touch);
  }

  /**
   * @brief Called to pass a controller motion message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller motion packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_MOTION_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_motion_t motion {
      {gamepad.id, packet->controllerNumber},
      packet->motionType,
      from_netfloat(packet->x),
      from_netfloat(packet->y),
      from_netfloat(packet->z),
    };

    platf::gamepad_motion(platf_input, motion);
  }

  /**
   * @brief Called to pass a controller battery message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller battery packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_BATTERY_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_battery_t battery {
      {gamepad.id, packet->controllerNumber},
      packet->batteryState,
      packet->batteryPercentage
    };

    platf::gamepad_battery(platf_input, battery);
  }

  /**
   * @brief Forward a client input packet directly to the platform backend.
   *
   * @param input Platform input backend that receives the event.
   * @param packet Protocol packet being processed.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';

      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];

    // If this is an event for a new gamepad, create the gamepad now. Ideally, the client would
    // send a controller arrival instead of this but it's still supported for legacy clients.
    if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
      if (alloc_gamepad(input, packet->controllerNumber, {}) < 0) {
        return;
      }
    } else if (!(packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id >= 0) {
      // If this is the final event for a gamepad being removed, free the gamepad and return.
      ::input::free_gamepad(platf_input, gamepad.id);
      gamepad.id = -1;
      return;
    }

    // If this gamepad has not been initialized, ignore it.
    // This could happen when platf::alloc_gamepad fails
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    std::uint16_t bf = packet->buttonFlags;
    std::uint32_t bf2 = packet->buttonFlags2;
    platf::gamepad_state_t gamepad_state {
      bf | (bf2 << 16),
      packet->leftTrigger,
      packet->rightTrigger,
      packet->leftStickX,
      packet->leftStickY,
      packet->rightStickX,
      packet->rightStickY
    };

    auto bf_new = gamepad_state.buttonFlags;
    switch (gamepad.back_button_state) {
      case button_state_e::UP:
        if (!(platf::BACK & bf_new)) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags &= ~platf::BACK;
        break;
      case button_state_e::DOWN:
        if (platf::BACK & bf_new) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags |= platf::BACK;
        break;
      case button_state_e::NONE:
        break;
    }

    bf = gamepad_state.buttonFlags ^ gamepad.gamepad_state.buttonFlags;
    bf_new = gamepad_state.buttonFlags;

    if (platf::BACK & bf) {
      if (platf::BACK & bf_new) {
        // Don't emulate home button if timeout < 0
        if (config::input.back_button_timeout >= 0ms) {
          auto f = [input, controller = packet->controllerNumber]() {
            auto &gamepad = input->gamepads[controller];

            auto &state = gamepad.gamepad_state;

            // Force the back button up
            gamepad.back_button_state = button_state_e::UP;
            state.buttonFlags &= ~platf::BACK;
            platf::gamepad_update(platf_input, gamepad.id, state);

            // Press Home button
            state.buttonFlags |= platf::HOME;
            platf::gamepad_update(platf_input, gamepad.id, state);

            // Release the Home button on a second delayed task instead of sleeping here: this
            // lambda runs on the single task pool worker, and a sleep would hold every queued
            // keyboard and mouse event behind it for the whole hold period.
            auto release_home = [input, controller]() {
              auto &gamepad = input->gamepads[controller];

              // The slot may already have been freed if the client disconnected while Home was held
              if (gamepad.id < 0) {
                gamepad.home_release_id = nullptr;
                return;
              }

              auto &state = gamepad.gamepad_state;
              state.buttonFlags &= ~platf::HOME;
              platf::gamepad_update(platf_input, gamepad.id, state);

              gamepad.home_release_id = nullptr;
            };

            gamepad.home_release_id = task_pool.pushDelayed(std::move(release_home), 100ms).task_id;

            gamepad.back_timeout_id = nullptr;
          };

          gamepad.back_timeout_id = task_pool.pushDelayed(std::move(f), config::input.back_button_timeout).task_id;
        }
      } else if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
    }

    platf::gamepad_update(platf_input, gamepad.id, gamepad_state);

    gamepad.gamepad_state = gamepad_state;
  }

  /**
   * @brief Validate the declared and available sizes of a fixed-size input packet.
   *
   * @tparam Packet Protocol packet structure.
   * @param packet Raw packet bytes.
   * @param declared_size Packet size declared after the size field.
   * @return True when both sizes safely contain the fixed packet structure.
   */
  template<typename Packet>
  bool validate_fixed_input_packet(std::span<const std::uint8_t> packet, std::uint32_t declared_size) {
    if (constexpr auto expected_size = static_cast<std::uint32_t>(sizeof(Packet) - sizeof(std::uint32_t)); declared_size != expected_size) {
      return false;
    }
    return packet.size() >= sizeof(Packet);
  }

  /**
   * @brief Validate an input packet before any typed access or batching.
   *
   * @param packet Raw packet bytes.
   * @param parsed_header Optional destination for the safely copied packet header.
   * @return True when the packet is large enough for its declared and protocol-specific fields.
   */
  bool validate_input_packet(std::span<const std::uint8_t> packet, NV_INPUT_HEADER *parsed_header = nullptr) {
    if (packet.size() < sizeof(NV_INPUT_HEADER)) {
      return false;
    }

    NV_INPUT_HEADER header {};
    std::memcpy(&header, packet.data(), sizeof(header));
    if (parsed_header) {
      *parsed_header = header;
    }

    const auto declared_size = util::endian::big(header.size);
    if (declared_size < sizeof(header.magic) || declared_size > packet.size() - sizeof(header.size)) {
      return false;
    }

    switch (util::endian::little(header.magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return validate_fixed_input_packet<NV_REL_MOUSE_MOVE_PACKET>(packet, declared_size);
      case MOUSE_MOVE_ABS_MAGIC:
        return validate_fixed_input_packet<NV_ABS_MOUSE_MOVE_PACKET>(packet, declared_size);
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return validate_fixed_input_packet<NV_MOUSE_BUTTON_PACKET>(packet, declared_size);
      case SCROLL_MAGIC_GEN5:
        return validate_fixed_input_packet<NV_SCROLL_PACKET>(packet, declared_size);
      case SS_HSCROLL_MAGIC:
        return validate_fixed_input_packet<SS_HSCROLL_PACKET>(packet, declared_size);
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        return validate_fixed_input_packet<NV_KEYBOARD_PACKET>(packet, declared_size);
      case UTF8_TEXT_EVENT_MAGIC:
        return declared_size - sizeof(header.magic) <= UTF8_TEXT_EVENT_MAX_COUNT;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return validate_fixed_input_packet<NV_MULTI_CONTROLLER_PACKET>(packet, declared_size);
      case mac_trackpad_wire::magnify_magic:
      case mac_trackpad_wire::rotate_magic:
        return validate_fixed_input_packet<mac_trackpad_wire::packet_t>(packet, declared_size);
      case SS_TOUCH_MAGIC:
        return validate_fixed_input_packet<SS_TOUCH_PACKET>(packet, declared_size);
      case SS_PEN_MAGIC:
        return validate_fixed_input_packet<SS_PEN_PACKET>(packet, declared_size);
      case pen_wire::magic:
        return pen_wire::valid_size(packet.size(), header.size);
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        return validate_fixed_input_packet<SS_CONTROLLER_ARRIVAL_PACKET>(packet, declared_size);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return validate_fixed_input_packet<SS_CONTROLLER_TOUCH_PACKET>(packet, declared_size);
      case SS_CONTROLLER_MOTION_MAGIC:
        return validate_fixed_input_packet<SS_CONTROLLER_MOTION_PACKET>(packet, declared_size);
      case SS_CONTROLLER_BATTERY_MAGIC:
        return validate_fixed_input_packet<SS_CONTROLLER_BATTERY_PACKET>(packet, declared_size);
      default:
        return true;
    }
  }

  /**
   * @brief Enumerates supported batch result options.
   */
  enum class batch_result_e {
    batched,  ///< This entry was batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch with this entry
  };

  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX;
    short deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller state messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src) {
    // Do not allow batching if the active controllers change
    if (dest->activeGamepadMask != src->activeGamepadMask) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Do not allow batching if the button state changes on this controller
    if (dest->buttonFlags != src->buttonFlags || dest->buttonFlags2 != src->buttonFlags2) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE && src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two pen messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same type
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Do not allow batching if the button state changes
    if (dest->penButtons != src->penButtons) {
      return batch_result_e::terminate_batch;
    }

    // Do not batch beyond tool changes
    if (dest->toolType != src->toolType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE && src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller motion messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src) {
    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Batched events must be the same sensor
    if (dest->motionType != src->motionType) {
      return batch_result_e::not_batchable;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two input messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
    // We can only batch if the packet types are the same
    if (dest->magic != src->magic) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch certain message types
    switch (util::endian::little(dest->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return batch((PNV_REL_MOUSE_MOVE_PACKET) dest, (PNV_REL_MOUSE_MOVE_PACKET) src);
      case MOUSE_MOVE_ABS_MAGIC:
        return batch((PNV_ABS_MOUSE_MOVE_PACKET) dest, (PNV_ABS_MOUSE_MOVE_PACKET) src);
      case SCROLL_MAGIC_GEN5:
        return batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
      case SS_HSCROLL_MAGIC:
        return batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return batch((PNV_MULTI_CONTROLLER_PACKET) dest, (PNV_MULTI_CONTROLLER_PACKET) src);
      case SS_TOUCH_MAGIC:
#ifdef __APPLE__
        return batch_result_e::terminate_batch;
#else
        return batch((PSS_TOUCH_PACKET) dest, (PSS_TOUCH_PACKET) src);
#endif
      case SS_PEN_MAGIC:
        return batch((PSS_PEN_PACKET) dest, (PSS_PEN_PACKET) src);
      case pen_wire::magic:
        {
          auto dest_pen = reinterpret_cast<pen_wire::packet_t *>(dest);
          auto src_pen = reinterpret_cast<pen_wire::packet_t *>(src);
          auto result = batch(&dest_pen->pen, &src_pen->pen);
          if (result == batch_result_e::batched) {
            dest_pen->barrelRoll = src_pen->barrelRoll;
            dest_pen->reserved = src_pen->reserved;
          }
          return result;
        }
      case SS_CONTROLLER_TOUCH_MAGIC:
        return batch((PSS_CONTROLLER_TOUCH_PACKET) dest, (PSS_CONTROLLER_TOUCH_PACKET) src);
      case SS_CONTROLLER_MOTION_MAGIC:
        return batch((PSS_CONTROLLER_MOTION_PACKET) dest, (PSS_CONTROLLER_MOTION_PACKET) src);
      default:
        // Not a batchable message type
        return batch_result_e::terminate_batch;
    }
  }

  namespace {

    /**
     * @brief Windowed counters comparing arriving scroll packets with injected scroll events.
     *
     * Moonlight scroll packets are mergeable, so a backed-up input queue folds several packets
     * into a single platform event (see passthrough_next_message()). These counters tell the
     * client arrival rhythm and the injected rhythm apart, which decides whether a stutter comes
     * from the client or from host-side batching.
     */
    struct scroll_census_t {
      std::atomic<uint64_t> received {0};  ///< Scroll packets accepted from the client.
      std::atomic<uint64_t> received_gap_samples {0};  ///< Number of measured arrival intervals.
      std::atomic<uint64_t> received_gap_total_us {0};  ///< Sum of arrival intervals in microseconds.
      std::atomic<uint64_t> received_gap_max_us {0};  ///< Largest arrival interval in microseconds.
      std::atomic<uint64_t> received_ns {0};  ///< Monotonic timestamp of the last accepted packet.
      std::atomic<uint64_t> injected {0};  ///< Scroll events handed to the platform backend.
      std::atomic<uint64_t> injected_gap_samples {0};  ///< Number of measured injection intervals.
      std::atomic<uint64_t> injected_gap_total_us {0};  ///< Sum of injection intervals in microseconds.
      std::atomic<uint64_t> injected_gap_max_us {0};  ///< Largest injection interval in microseconds.
      std::atomic<uint64_t> injected_ns {0};  ///< Monotonic timestamp of the last injected event.
      std::atomic<uint64_t> merged_away {0};  ///< Packets folded into another packet by batch().
      std::atomic<uint64_t> window_ns {0};  ///< Monotonic timestamp where the current report window began.
      std::mutex report_lock;  ///< Keeps one report per window.

      /**
       * @brief Inter-arrival histogram for one side of the pipeline.
       *
       * Bucket edges in milliseconds are 4, 8, 12, 16, 25, 33 and 50, so a stream paced by a
       * 60Hz or 30Hz clock is told apart from one that arrives in bursts after a long stall.
       * A mean alone cannot separate those two, and they need different fixes.
       */
      struct gap_histogram_t {
        std::array<std::atomic<uint64_t>, 8> buckets;  ///< Counts per interval bucket.

        /// Record one interval sample in the bucket it falls into.
        void record(uint64_t value_us) {
          static constexpr uint64_t edges[] {4000, 8000, 12000, 16000, 25000, 33000, 50000};
          std::size_t bucket = 0;
          while (bucket + 1 < std::size(edges) && value_us >= edges[bucket]) {
            ++bucket;
          }
          buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        }

        /// Read and reset all bucket counts.
        std::array<uint64_t, 8> drain() {
          std::array<uint64_t, 8> drained {};
          for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
            drained[bucket] = buckets[bucket].exchange(0);
          }
          return drained;
        }
      };

      gap_histogram_t received_histogram;  ///< Interval buckets for packets arriving from the client.
      gap_histogram_t injected_histogram;  ///< Interval buckets for events handed to the platform.
    };

    scroll_census_t scroll_census;

    /**
     * @brief Read the monotonic clock in nanoseconds for census intervals.
     * @return Nanoseconds since the monotonic clock epoch.
     */
    uint64_t census_now_ns() {
      return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
      );
    }

    /**
     * @brief Whether scroll census logging is on.
     *
     * Shares the smooth-scrolling diagnostics switch (macos_smooth_scrolling_diagnostics, or the
     * LUMEN_SCROLL_DIAGNOSTICS environment variable) so one setting turns on every scroll probe.
     * Cached because it is consulted for every scroll packet.
     * @return True when census lines should be written.
     */
    bool scroll_census_enabled() {
      static const bool enabled = config::input.macos_smooth_scrolling_diagnostics != "disabled" || std::getenv("LUMEN_SCROLL_DIAGNOSTICS") != nullptr;
      return enabled;
    }

    /**
     * @brief Whether a packet magic describes vertical or horizontal scroll input.
     *
     * @param magic Packet magic already converted to host byte order.
     * @return True for scroll packets.
     */
    bool is_scroll_magic(int magic) {
      return magic == SCROLL_MAGIC_GEN5 || magic == SS_HSCROLL_MAGIC;
    }

    /**
     * @brief Merge one interval sample into a count/sum/maximum triple.
     *
     * @param samples Counter of samples.
     * @param total Running sum in microseconds.
     * @param maximum Running maximum in microseconds.
     * @param value_us Sample interval in microseconds.
     */
    void census_record_interval(std::atomic<uint64_t> &samples, std::atomic<uint64_t> &total, std::atomic<uint64_t> &maximum, uint64_t value_us) {
      samples.fetch_add(1, std::memory_order_relaxed);
      total.fetch_add(value_us, std::memory_order_relaxed);
      auto previous_max = maximum.load(std::memory_order_relaxed);
      while (value_us > previous_max && !maximum.compare_exchange_weak(previous_max, value_us, std::memory_order_relaxed)) {
      }
    }

    /**
     * @brief Emit and reset the census counters, at most once per second of scroll activity.
     */
    void report_scroll_census() {
      if (!scroll_census_enabled()) {
        return;
      }

      const auto now_ns = census_now_ns();
      auto window_ns = scroll_census.window_ns.load(std::memory_order_relaxed);
      if (window_ns == 0) {
        scroll_census.window_ns.store(now_ns, std::memory_order_relaxed);
        return;
      }
      if (now_ns - window_ns < 1'000'000'000) {
        return;
      }

      std::lock_guard<std::mutex> lock(scroll_census.report_lock);
      const auto received = scroll_census.received.exchange(0);
      const auto injected = scroll_census.injected.exchange(0);
      const auto merged_away = scroll_census.merged_away.exchange(0);
      const auto received_samples = scroll_census.received_gap_samples.exchange(0);
      const auto received_total_us = scroll_census.received_gap_total_us.exchange(0);
      const auto received_max_us = scroll_census.received_gap_max_us.exchange(0);
      const auto injected_samples = scroll_census.injected_gap_samples.exchange(0);
      const auto injected_total_us = scroll_census.injected_gap_total_us.exchange(0);
      const auto injected_max_us = scroll_census.injected_gap_max_us.exchange(0);
      const auto window_seconds = static_cast<double>(now_ns - window_ns) / 1'000'000'000.0;
      scroll_census.window_ns.store(now_ns, std::memory_order_relaxed);

      if (received == 0 && injected == 0) {
        return;
      }

      const auto received_gap_mean_ms = received_samples ? static_cast<double>(received_total_us) / received_samples / 1000.0 : 0.0;
      const auto injected_gap_mean_ms = injected_samples ? static_cast<double>(injected_total_us) / injected_samples / 1000.0 : 0.0;
      // Packets actually consumed in this window = one per injected event, plus the ones folded away.
      const auto packets_per_event = injected ? 1.0 + static_cast<double>(merged_away) / static_cast<double>(injected) : 0.0;

      const auto received_buckets = scroll_census.received_histogram.drain();
      const auto injected_buckets = scroll_census.injected_histogram.drain();

      BOOST_LOG(info)
        << "Scroll census ["sv << window_seconds << "s]: client packets="sv << received
        << " avg_gap="sv << received_gap_mean_ms << "ms max_gap="sv << static_cast<double>(received_max_us) / 1000.0 << "ms"
        << " | injected events="sv << injected
        << " avg_gap="sv << injected_gap_mean_ms << "ms max_gap="sv << static_cast<double>(injected_max_us) / 1000.0 << "ms"
        << " | packets_per_event="sv << packets_per_event
        << " merged_away="sv << merged_away;

      // Bucket edges in milliseconds: <4, 4-8, 8-12, 12-16, 16-25, 25-33, 33-50, >50.
      // One BOOST_LOG statement is one log record, so the joined buckets are assembled into a
      // single string first; streaming them from a loop would emit one line per bucket.
      std::string gap_report {"Scroll census gaps [ms <4/4-8/8-12/12-16/16-25/25-33/33-50/>50]:"};
      const std::pair<std::string_view, const std::array<uint64_t, 8> &> sides[] {
        {"client", received_buckets},
        {"injected", injected_buckets},
      };
      for (const auto &[name, buckets] : sides) {
        gap_report += " ";
        gap_report += name;
        gap_report += "=";
        for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
          if (bucket != 0) {
            gap_report += '/';
          }
          gap_report += std::to_string(buckets[bucket]);
        }
      }
      BOOST_LOG(info) << gap_report;
    }

    /**
     * @brief Record one scroll packet accepted from the client, before batching.
     */
    void note_scroll_packet_received() {
      if (!scroll_census_enabled()) {
        return;
      }
      const auto now_ns = census_now_ns();
      const auto previous_ns = scroll_census.received_ns.exchange(now_ns, std::memory_order_relaxed);
      if (previous_ns != 0) {
        const auto gap_us = (now_ns - previous_ns) / 1000;
        census_record_interval(scroll_census.received_gap_samples, scroll_census.received_gap_total_us, scroll_census.received_gap_max_us, gap_us);
        scroll_census.received_histogram.record(gap_us);
      }
      scroll_census.received.fetch_add(1, std::memory_order_relaxed);
      report_scroll_census();
    }

    /**
     * @brief Record one scroll event handed to the platform backend after batching.
     *
     * @param merged_away Count of extra packets folded into this event.
     */
    void note_scroll_event_injected(uint64_t merged_away) {
      if (!scroll_census_enabled()) {
        return;
      }
      const auto now_ns = census_now_ns();
      const auto previous_ns = scroll_census.injected_ns.exchange(now_ns, std::memory_order_relaxed);
      if (previous_ns != 0) {
        const auto gap_us = (now_ns - previous_ns) / 1000;
        census_record_interval(scroll_census.injected_gap_samples, scroll_census.injected_gap_total_us, scroll_census.injected_gap_max_us, gap_us);
        scroll_census.injected_histogram.record(gap_us);
      }
      scroll_census.injected.fetch_add(1, std::memory_order_relaxed);
      if (merged_away != 0) {
        scroll_census.merged_away.fetch_add(merged_away, std::memory_order_relaxed);
      }
      report_scroll_census();
    }

  }  // namespace

  /**
   * @brief Called on a thread pool thread to process an input message.
   * @param input The input context pointer.
   * @param generation Session generation captured when the task was queued.
   */
  void passthrough_next_message(std::shared_ptr<input_t> input, std::uint64_t generation) {
    // 'entry' backs the 'payload' pointer, so they must remain in scope together
    std::vector<uint8_t> entry;
    PNV_INPUT_HEADER payload;
    uint64_t scroll_merged_away = 0;  ///< Scroll packets folded into the one being sent.

    // Lock the input queue while batching, but release it before sending
    // the input to the OS. This avoids potentially lengthy lock contention
    // in the control stream thread while input is being processed by the OS.
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);

      // If all entries have already been processed, nothing to do
      if (input->input_stopped || generation != input->input_generation || input->input_queue.empty()) {
        return;
      }

      // Pop off the first entry, which we will send
      entry = input->input_queue.front();
      payload = (PNV_INPUT_HEADER) entry.data();
      input->input_queue.pop_front();

      // Try to batch with remaining items on the queue
      auto i = input->input_queue.begin();
      while (i != input->input_queue.end()) {
        auto batchable_entry = *i;
        auto batchable_payload = (PNV_INPUT_HEADER) batchable_entry.data();

        auto batch_result = batch(payload, batchable_payload);
        if (batch_result == batch_result_e::terminate_batch) {
          // Stop batching
          break;
        } else if (batch_result == batch_result_e::batched) {
          if (is_scroll_magic(util::endian::little(payload->magic))) {
            // This scroll packet's motion is now part of the event above, and its own
            // intermediate position is gone.
            ++scroll_merged_away;
          }

          // Erase this entry since it was batched
          i = input->input_queue.erase(i);
        } else {
          // We couldn't batch this entry, but try to batch later entries.
          i++;
        }
      }
    }

    // Print the final input packet
    input::print((void *) payload);

    if (is_scroll_magic(util::endian::little(payload->magic))) {
      note_scroll_event_injected(scroll_merged_away);
    }

    // Send the batched input to the OS
    const auto type = util::endian::little(payload->magic);
    switch (type) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        passthrough(input, (PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        passthrough(input, (PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        passthrough(input, (PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        passthrough(input, (PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        passthrough(input, (PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        passthrough(input, (PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        passthrough(static_cast<const NV_UNICODE_PACKET *>(static_cast<const void *>(payload)));
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        passthrough(input, (PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
#ifdef __APPLE__
      case mac_trackpad_wire::magnify_magic:
      case mac_trackpad_wire::rotate_magic:
        passthrough(input, *reinterpret_cast<const mac_trackpad_wire::packet_t *>(payload), type == mac_trackpad_wire::rotate_magic);
        break;
#endif
      case SS_TOUCH_MAGIC:
        passthrough(input, (PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        passthrough(input, (PSS_PEN_PACKET) payload);
        break;
      case pen_wire::magic:
        {
          auto packet = reinterpret_cast<pen_wire::packet_t *>(payload);
          passthrough(input, &packet->pen, pen_wire::decode_roll(packet->barrelRoll));
          break;
        }
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        passthrough(input, (PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        passthrough(input, (PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        passthrough(input, (PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        passthrough(input, (PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
    }
  }

  /**
   * @brief Called on the control stream thread to queue an input message.
   * @param input The input context pointer.
   * @param input_data The input message.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    NV_INPUT_HEADER header {};
    if (!validate_input_packet(input_data, &header)) {
      if (util::endian::little(header.magic) == pen_wire::magic) {
        std::lock_guard lock {input->input_queue_lock};
        const auto now = std::chrono::steady_clock::now();
        if (now < input->next_invalid_pen_warning) {
          return;
        }
        input->next_invalid_pen_warning = now + 5s;
        BOOST_LOG(warning) << "Dropping malformed barrel-roll pen packet: declared payload "
                           << util::endian::big(header.size) << " (expected 36), actual bytes "
                           << input_data.size() << " (expected 40)";
        return;
      }
      if (input_data.size() >= sizeof(header)) {
        BOOST_LOG(warning)
          << "Dropping malformed input packet type ["sv
          << util::hex(util::endian::little(header.magic)).to_string_view()
          << "] with declared payload size ["sv << util::endian::big(header.size)
          << "] and actual size ["sv << input_data.size() << ']';
      } else {
        BOOST_LOG(warning) << "Dropping malformed input packet with actual size ["sv << input_data.size() << ']';
      }
      return;
    }

    if (is_scroll_magic(util::endian::little(header.magic))) {
      note_scroll_packet_received();
    }

    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      if (input->input_stopped) {
        return;
      }
      const auto magic = util::endian::little(header.magic);
      if (magic == SS_PEN_MAGIC) {
        input->pen_roll_probe.reset();
      } else if (magic == pen_wire::magic) {
        auto &packet = *reinterpret_cast<pen_wire::packet_t *>(input_data.data());
        // Observe every packet before MOVE/HOVER coalescing can discard hardware changes.
        packet.barrelRoll = util::endian::little(input->pen_roll_probe.select(
          pen_wire::decode_roll(packet.barrelRoll),
          pen_wire::decode_roll(packet.pen.rotation)
        ));
      }
      input->input_queue.push_back(std::move(input_data));
      task_pool.push(passthrough_next_message, input, input->input_generation);
    }
  }

  /**
   * @brief Release every pressed mouse button tracked by Sunshine.
   */
  void reset_mouse_buttons() {
    for (int button = 0; button < mouse_press.size(); ++button) {
      if (mouse_press[button]) {
        platf::button_mouse(platf_input, button, true);
        mouse_press[button] = false;
      }
    }
  }

  /**
   * @brief Release every pressed keyboard key tracked by Sunshine.
   */
  void reset_keyboard_keys() {
    for (auto &[key, pressed] : key_press) {
      if (pressed) {
        // key_press is keyed on the client's unmapped virtual-key code, but the press was
        // emitted through map_keycode(). Release the host key that actually went down,
        // otherwise a remapped modifier stays latched after the client disconnects.
        emit_keyboard_update(map_keycode(vk_from_kpid(key) & 0x00FF), true, flags_from_kpid(key));
        pressed = false;
      }
    }
  }

  /**
   * @brief Neutralize retained gamepads without destroying their virtual devices.
   *
   * @param input Retained stream input state to neutralize.
   */
  void reset_gamepads(const std::shared_ptr<input_t> &input) {
    for (int client_index = 0; client_index < input->gamepads.size(); ++client_index) {
      auto &gamepad = input->gamepads[client_index];
      if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
      if (gamepad.home_release_id) {
        task_pool.cancel(gamepad.home_release_id);
        gamepad.home_release_id = nullptr;
      }
      if (gamepad.id >= 0) {
        platf::gamepad_update(platf_input, gamepad.id, {});
        platf::gamepad_touch(
          platf_input,
          {{gamepad.id, static_cast<std::uint8_t>(client_index)}, LI_TOUCH_EVENT_CANCEL_ALL, 0, 0.0F, 0.0F, 0.0F}
        );
      }
      gamepad.gamepad_state = {};
      gamepad.back_button_state = button_state_e::NONE;
    }
  }

  /**
   * @brief Reset all pressed input state for a disconnected stream.
   *
   * @param input Retained stream input state to reset.
   */
  void reset_input_state(const std::shared_ptr<input_t> &input) {
    reset_mouse_buttons();
    reset_keyboard_keys();
    reset_gamepads(input);
#ifdef __APPLE__
    input->scroll_brake_left_down = false;
    if (input->mac_magnify_active) {
      platf::macos::touch_magnify(platf::macos::gesture_phase::cancelled, 0);
      input->mac_magnify_active = false;
    }
    if (input->mac_rotate_active) {
      platf::macos::touch_rotate(platf::macos::gesture_phase::cancelled, 0);
      input->mac_rotate_active = false;
    }
    auto &touch = input->native_touch;
    release_native_touch_left(touch);
    finish_native_touch_magnify(touch, true);
    if (touch.sample_task) {
      task_pool.cancel(touch.sample_task);
    }
    release_native_touch_right(touch);
    touch = {};
    // Retained client contexts survive disconnect, so their pen must explicitly leave range.
    platf::pen_input_t pen {};
    pen.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
    emit_pen(input, {}, pen);
#endif
  }

  /**
   * @brief Reset the object to its initial empty state.
   */
  void reset(std::shared_ptr<input_t> &input) {
    {
      std::lock_guard lock {input->input_queue_lock};
      input->input_stopped = true;
      ++input->input_generation;
      input->input_queue.clear();
      input->pen_roll_probe.reset();
      // Queue cleanup before any resumed-session message can be scheduled.
      task_pool.push(reset_input_state, input);
    }
    task_pool.cancel(key_press_repeat_id);
    task_pool.cancel(input->mouse_left_button_timeout);
  }

  void terminate_gamepads() {
    retained_input_map_t inputs;
    {
      auto &state = retained_input_state();
      std::lock_guard lock {state.mutex};
      state.inputs.swap(inputs);
    }

    if (!inputs.empty()) {
      dispatch_input_task([inputs = std::move(inputs)]() {
        destroy_gamepads(inputs);
      });
    }
  }

  void terminate_gamepads(const std::string_view session_id) {
    std::shared_ptr<input_t> input;
    {
      auto &state = retained_input_state();
      std::lock_guard lock {state.mutex};
      const auto iter = state.inputs.find(session_id);
      if (iter == state.inputs.end()) {
        return;
      }

      input = std::move(iter->second);
      state.inputs.erase(iter);
    }

    dispatch_input_task([input = std::move(input)]() {
      destroy_gamepads(input);
    });
  }

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    /**
     * @brief Destroy the input subsystem deinitializer.
     */
    ~deinit_t() override {
      retained_input_map_t inputs;
      {
        auto &state = retained_input_state();
        std::lock_guard lock {state.mutex};
        state.inputs.swap(inputs);
      }
      destroy_gamepads(inputs);
      platf_input.reset();
    }
  };

  /**
   * @brief Initialize the platform input backend.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init() {
    platf_input = platf::input();

    return std::make_unique<deinit_t>();
  }

  /**
   * @brief Probe connected gamepads and update input capability state.
   */
  bool probe_gamepads() {
    const auto &gamepads = platf::supported_gamepads(std::addressof(platf_input));
    return std::ranges::none_of(gamepads, [](const auto &gamepad) {
      return gamepad.is_enabled && gamepad.name != "auto";
    });
  }

  void refresh_virtual_input() {
    dispatch_input_task([]() {
      if (platf_input) {
        task_pool.cancel(key_press_repeat_id);
        key_press_repeat_id = nullptr;
        reset_mouse_buttons();
        reset_keyboard_keys();
        auto &context = platf::virtualhid::get_input_context(platf_input);
        context.refresh_keyboard();
        context.refresh_mouse();
      }
    });
  }

  /**
   * @brief Allocate and initialize platform input state for a stream.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id) {
    std::shared_ptr<input_t> input;
    bool resumed = false;
    {
      auto &state = retained_input_state();
      std::lock_guard lock {state.mutex};
      const auto iter = state.inputs.find(session_id);
      if (iter != state.inputs.end()) {
        input = iter->second;
        resumed = true;
      } else {
        input = std::make_shared<input_t>(
          mail->event<input::touch_port_t>(mail::touch_port),
          mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback)
        );
        state.inputs.try_emplace(std::move(session_id), input);
      }
    }

    {
      std::lock_guard lock {input->input_queue_lock};
      input->input_stopped = false;
    }
    if (resumed) {
      dispatch_input_task([input, mail = std::move(mail)]() {
        rebind_input(input, mail);
      });
    }

    // Workaround to ensure new frames will be captured when a client connects
    task_pool.pushDelayed([]() {
      platf::move_mouse(platf_input, 1, 1);
      platf::move_mouse(platf_input, -1, -1);
    },
                          100ms);

    return input;
  }

#ifdef SUNSHINE_TESTS
  namespace testing {
    void set_platform_input(platf::input_t input) {
      terminate_gamepads();
      platf_input = std::move(input);
      gamepadMask.reset();
    }

    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata) {
      return ::input::alloc_gamepad(input, client_index, metadata);
    }

    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index) {
      if (!input || client_index >= input->gamepads.size()) {
        return -1;
      }
      return input->gamepads[client_index].id;
    }

    void set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink) {
      keyboard_sink() = std::move(sink);
    }

    void send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code, std::uint8_t modifiers, std::uint8_t flags, bool release) {
      const std::uint32_t magic = release ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC;

      NV_KEYBOARD_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little(magic);
      packet.keyCode = static_cast<short>(key_code);
      packet.modifiers = static_cast<char>(modifiers);
      packet.flags = static_cast<char>(flags);

      // Keyboard packets are never batched, so this matches passthrough_next_message().
      ::input::passthrough(input, &packet);
    }

    void reset_keyboard_state() {
      task_pool.cancel(key_press_repeat_id);
      key_press_repeat_id = nullptr;
      key_press.clear();
    }

    void release_held_keys() {
      reset_keyboard_keys();
    }

    void set_pen_sink(std::function<void(const platf::touch_port_t &, const platf::pen_input_t &)> sink) {
      pen_sink = std::move(sink);
    }

    void process_next_input(const std::shared_ptr<input_t> &input) {
      std::uint64_t generation;
      {
        std::lock_guard lock {input->input_queue_lock};
        generation = input->input_generation;
      }
      passthrough_next_message(input, generation);
    }

    bool is_valid_input_packet(std::span<const std::uint8_t> packet) {
      return ::input::validate_input_packet(packet);
    }

    std::size_t queued_input_packet_count(const std::shared_ptr<input_t> &input) {
      if (!input) {
        return 0;
      }

      std::lock_guard lock {input->input_queue_lock};
      return input->input_queue.size();
    }
  }  // namespace testing
#endif
}  // namespace input
