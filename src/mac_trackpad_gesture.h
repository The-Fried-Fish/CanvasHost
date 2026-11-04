/**
 * @file mac_trackpad_gesture.h
 * @brief Incremental gesture events for macOS trackpad simulation.
 */
#pragma once

#include <cstdint>
#include <moonlight-common-c/src/Input.h>

namespace input::mac_trackpad_wire {
  constexpr std::uint32_t magnify_magic = 0x5500000C;  ///< macMagnify input packet ID.
  constexpr std::uint32_t rotate_magic = 0x5500000E;  ///< macRotate input packet ID.
#pragma pack(push, 1)
  /** @brief A gesture phase and little-endian float increment. */
  struct packet_t {
    NV_INPUT_HEADER header;  ///< Input packet header.
    std::uint8_t phase;  ///< Begin, change, end or cancel.
    netfloat amount;  ///< Incremental magnification or rotation in degrees.
  };
#pragma pack(pop)
  static_assert(sizeof(packet_t) == 13);
}
