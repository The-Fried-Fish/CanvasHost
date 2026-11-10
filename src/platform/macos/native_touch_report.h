/**
 * @file
 * @brief Contact state and HID report layout for the macOS virtual touchscreen.
 */
#pragma once

#include <stdint.h>

namespace mac_touch {
  constexpr unsigned max_contacts = 16;  ///< Number of simultaneous contacts in the HID descriptor.
  constexpr uint8_t touching = 1;  ///< Contact is in range and touching the screen.
  constexpr uint8_t changed = 2;  ///< Touch state changed in this report.
  constexpr uint8_t moved = 4;  ///< Contact position changed in this report.

  /** @brief One contact in the little-endian HID report. */
  struct contact_t {
    uint8_t flags;  ///< Touch state and transition flags.
    uint8_t id;  ///< Stable slot number, or zero for an unused slot.
    uint8_t x[2];  ///< Normalized horizontal coordinate in unsigned 0.16 format.
    uint8_t y[2];  ///< Normalized vertical coordinate in unsigned 0.16 format.
  };

  /** @brief Snapshot of active contacts and any contact ending in this frame. */
  struct report_t {
    contact_t contacts[max_contacts];  ///< Slots in descriptor order.
  };

  static_assert(sizeof(report_t) == 6 * max_contacts);
  /** @brief Digitizer report submitted to the macOS HID driver. */
  struct hid_report_t {
    contact_t contacts[max_contacts];  ///< Slots with Tip Switch, In Range and Confidence bits.
    uint8_t count;  ///< Number of populated slots, including contacts ending in this frame.
  };

  static_assert(sizeof(hid_report_t) == sizeof(report_t) + 1);

  /**
   * @brief Encodes a contact snapshot as a standard touchscreen HID report.
   * @param report Snapshot from the streaming host.
   * @return HID report in the device's descriptor layout.
   */
  inline hid_report_t encode_hid_report(const report_t &report) {
    constexpr uint8_t confidence = 4;
    constexpr uint8_t tip_and_range = 3;
    hid_report_t hid {};
    for (unsigned i = 0; i < max_contacts; ++i) {
      const auto &contact = report.contacts[i];
      if (contact.id) {
        hid.contacts[i] = contact;
        hid.contacts[i].flags = confidence | ((contact.flags & touching) ? tip_and_range : 0);
        ++hid.count;
      }
    }
    return hid;
  }
}  // namespace mac_touch
