/**
 * @file native_touch_workaround.h
 * @brief Motion classification for the macOS nativeTouchWorkaround.
 */
#pragma once

#include <ApplicationServices/ApplicationServices.h>
#include <chrono>
#include <cmath>

namespace platf::macos {
  constexpr std::chrono::milliseconds touch_cursor_wait {50};  ///< Wait for a second finger before starting cursor movement.
  constexpr std::chrono::milliseconds touch_gesture_decision_window {36};  ///< Average initial motion before locking the gesture.
  constexpr std::chrono::milliseconds touch_gesture_output_interval {8};  ///< Batch interval after the gesture is locked.
  constexpr double touch_scroll_gain = 1.2;  ///< Shared horizontal and vertical scroll output gain.
  constexpr double magnify_speed_difference = 160.0;  ///< Required pinch speed advantage in Quartz points per second.

  constexpr std::chrono::milliseconds touch_double_tap_window {250};  ///< Maximum interval from first up to second down.
  constexpr double touch_double_tap_distance = 0.025;  ///< Maximum normalized distance between taps.

  /**
   * @brief Decide whether a confirmed tap continues the previous click.
   * @param previous Previous click position.
   * @param current Confirmed tap position.
   * @param extent Display dimensions.
   * @param gap Time from the previous touch up to the current touch down.
   * @return Whether to reuse the previous click location and emit a double click.
   */
  inline bool touch_is_double_tap(CGPoint previous, CGPoint current, CGSize extent, std::chrono::steady_clock::duration gap) {
    return gap >= std::chrono::steady_clock::duration::zero() && gap <= touch_double_tap_window &&
           std::hypot((current.x - previous.x) / extent.width, (current.y - previous.y) / extent.height) <= touch_double_tap_distance;
  }

  /**
   * @brief Check whether the contact is still within the tap dead zone.
   * @param origin Initial contact position.
   * @param position Current contact position.
   * @return Whether the displacement is within the dead zone in Quartz points.
   */
  inline bool touch_is_stationary(CGPoint origin, CGPoint position) {
    return std::hypot(position.x - origin.x, position.y - origin.y) <= std::hypot(3.0, 3.0);
  }

  /**
   * @brief Check the normalized displacement required to start dragging.
   * @param origin Initial contact position.
   * @param position Current contact position.
   * @param extent Capture display dimensions.
   * @return Whether the displacement exceeds the drag threshold.
   */
  inline bool touch_exceeds_drag_threshold(CGPoint origin, CGPoint position, CGSize extent) {
    return std::hypot((position.x - origin.x) / extent.width, (position.y - origin.y) / extent.height) > 0.02;
  }

  /** @brief Output mode locked for a touch sequence until all contacts lift. */
  enum class touch_gesture_mode { undecided, magnify, vertical, horizontal };

  /** @brief Result of classifying one sample of two-finger movement. */
  struct touch_motion_t {
    double x;  ///< Horizontal scroll displacement.
    double y;  ///< Vertical scroll displacement.
    double magnification;  ///< Signed change in finger separation.
  };

  /**
   * @brief Measure signed centroid and separation changes between contact updates.
   * @param previous_a Previous first contact.
   * @param previous_b Previous second contact.
   * @param a Current first contact.
   * @param b Current second contact.
   * @return Signed displacement components.
   */
  inline touch_motion_t measure_touch_motion(CGPoint previous_a, CGPoint previous_b, CGPoint a, CGPoint b) {
    return {
      ((a.x - previous_a.x) + (b.x - previous_b.x)) / 2,
      ((a.y - previous_a.y) + (b.y - previous_b.y)) / 2,
      std::hypot(a.x - b.x, a.y - b.y) - std::hypot(previous_a.x - previous_b.x, previous_a.y - previous_b.y),
    };
  }

  /**
   * @brief Compare time-weighted average centroid and separation velocities.
   * @param displacement Accumulated signed changes across the sample window.
   * @param sample_seconds Duration of the sample window.
   * @param mode Sequence mode to select or retain.
   * @return Displacement projected onto the selected mode.
   */
  inline touch_motion_t classify_touch_motion(touch_motion_t displacement, double sample_seconds, touch_gesture_mode &mode) {
    const auto [dx, dy, separation] = displacement;
    if (mode == touch_gesture_mode::undecided) {
      if (dx == 0 && dy == 0 && separation == 0) {
        return {};
      }
      if (std::abs(separation) - std::hypot(dx, dy) >= magnify_speed_difference * sample_seconds) {
        mode = touch_gesture_mode::magnify;
      } else {
        mode = std::abs(dx) > std::abs(dy) ? touch_gesture_mode::horizontal : touch_gesture_mode::vertical;
      }
    }
    return {
      mode == touch_gesture_mode::horizontal ? dx : 0,
      mode == touch_gesture_mode::vertical ? dy : 0,
      mode == touch_gesture_mode::magnify ? separation : 0,
    };
  }
}
