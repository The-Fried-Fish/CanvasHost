/**
 * @file
 * @brief Per-stream native touchscreen state on macOS.
 */
#pragma once

#include "native_touch_report.h"

#include <array>
#include <cstdint>
#include <optional>
#include <CoreFoundation/CoreFoundation.h>

namespace platf::macos {
  /** @brief Tracks client contact IDs and produces full HID snapshots. */
  class touch_contacts_t {
  public:
    /**
     * @brief Applies one contact update.
     * @param type Moonlight touch event type.
     * @param id Client contact ID.
     * @param position Normalized position, when available.
     * @return Report to send, or no report for an ignored event.
     */
    std::optional<mac_touch::report_t> update(uint8_t type, uint32_t id, std::optional<std::array<float, 2>> position);

    /** @brief Returns whether any contacts are still down. */
    bool active() const;

  private:
    /** @brief Client ID and current wire state of a slot. */
    struct slot_t {
      uint32_t client_id;  ///< ID from the client packet.
      mac_touch::contact_t contact;  ///< Latest HID contact.
    };

    std::array<slot_t, mac_touch::max_contacts> slots {};  ///< Active contacts by HID slot.
  };

  /** @brief Owns the virtual touchscreen and contact state for one streaming session. */
  class native_touch_t {
  public:
    native_touch_t() = default;
    ~native_touch_t();
    native_touch_t(const native_touch_t &) = delete;
    native_touch_t &operator=(const native_touch_t &) = delete;

    /** @brief Returns whether this session currently owns a virtual touchscreen. */
    bool connected() const;

    /**
     * @brief Starts a virtual touchscreen on the specified display.
     * @param display Core Graphics display ID.
     * @return True if the device and driver are ready.
     */
    bool connect(uint32_t display);

    /**
     * @brief Sends one client contact update.
     * @param display Display ID for this packet, or zero when unavailable.
     * @param type Moonlight touch event type.
     * @param id Client contact ID.
     * @param position Normalized display position.
     */
    void update(uint32_t display, uint8_t type, uint32_t id, std::optional<std::array<float, 2>> position);

    /** @brief Releases every contact and destroys the virtual touchscreen. */
    void reset();

  private:
    void disconnect();  ///< Releases the virtual touchscreen.
    bool send(const mac_touch::report_t &report);  ///< Writes a complete HID report.
    touch_contacts_t contacts_;  ///< Contact state associated with the device.
    uint32_t display_ = 0;  ///< Display currently bound to the device.
    CFTypeRef device_ = nullptr;  ///< Owned virtual HID device.
  };
}  // namespace platf::macos
