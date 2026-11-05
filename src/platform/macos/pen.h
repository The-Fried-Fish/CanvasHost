/**
 * @file src/platform/macos/pen.h
 * @brief Per-client native Quartz tablet injection, without a virtual HID driver.
 */
#pragma once

#include "src/apple_pencil_side_buttons.h"
#include "src/platform/common.h"

#include <ApplicationServices/ApplicationServices.h>
#include <functional>
#include <memory>
#include <optional>

namespace platf::macos {
  /**
   * @brief Resolve a normalized pen location on the capture display.
   * @param viewport Capture display metadata.
   * @param x Normalized horizontal location.
   * @param y Normalized vertical location.
   * @return Quartz desktop position, or no position if the display is unavailable.
   */
  std::optional<CGPoint> pen_display_location(const touch_port_t &viewport, float x, float y);

  /**
   * @brief Create a Quartz event for a native Apple Pencil gesture.
   * @param gesture Double tap or squeeze phase.
   * @param position Target location in Quartz desktop coordinates.
   * @return Owned event, or null on failure.
   */
  CGEventRef create_apple_pencil_gesture(input::apple_pencil_wire::event_t gesture, CGPoint position);

  /**
   * @brief Post a native Apple Pencil gesture to Quartz.
   * @param gesture Double tap or squeeze phase.
   * @param position Target location in Quartz desktop coordinates.
   * @return Whether the event was created and submitted.
   */
  bool post_apple_pencil_gesture(input::apple_pencil_wire::event_t gesture, CGPoint position);

  /** @brief Own a pen's proximity, stroke, and tablet identity for one client. */
  class pen_device_t {
  public:
    using event_sink_t = std::function<void(CGEventRef)>;  ///< Receives a borrowed event synchronously.

    /**
     * @brief Create an independent tablet identity.
     * @param sink Optional recorder; an empty sink posts to the system HID event tap.
     */
    explicit pen_device_t(event_sink_t sink = {});
    /** @brief Release the tip and leave proximity before destroying the device. */
    ~pen_device_t();

    pen_device_t(const pen_device_t &) = delete;
    pen_device_t &operator=(const pen_device_t &) = delete;

    /**
     * @brief Translate a protocol sample into native tablet mouse/proximity events.
     * @param viewport Desktop bounds in Quartz coordinates.
     * @param pen Pen sample; cancellation works even without a valid viewport.
     */
    void update(const touch_port_t &viewport, const pen_input_t &pen);

#ifdef SUNSHINE_TESTS
    /**
     * @brief Refill the drag token without starting a display link.
     * @param refreshed_at Simulated refresh time.
     */
    void signal_display_refresh(std::chrono::steady_clock::time_point refreshed_at);
#endif

  private:
    struct state_t;  ///< Native event source and the last delivered tablet state.
    std::unique_ptr<state_t> state_;  ///< Per-client implementation.
  };
}  // namespace platf::macos
