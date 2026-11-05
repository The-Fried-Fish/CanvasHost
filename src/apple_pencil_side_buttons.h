/**
 * @file apple_pencil_side_buttons.h
 * @brief Wire format for Apple Pencil gestures.
 */
#pragma once

#include <cstdint>
#include <moonlight-common-c/src/Input.h>

namespace input::apple_pencil_wire {
  constexpr std::uint32_t magic = 0x5500000D;  ///< Apple Pencil gesture packet ID.

  /** @brief Discrete double tap and continuous squeeze phases. */
  enum class event_t : std::uint8_t {
    double_tap = 0,
    squeeze_begin = 1,
    squeeze_change = 2,
    squeeze_end = 4,
    squeeze_cancel = 8,
  };

#pragma pack(push, 1)
  /** @brief Gesture and normalized video coordinates. */
  struct packet_t {
    NV_INPUT_HEADER header;  ///< Input packet header.
    event_t event;  ///< Pencil gesture or squeeze phase.
    netfloat x;  ///< Horizontal position.
    netfloat y;  ///< Vertical position.
  };
#pragma pack(pop)
  static_assert(sizeof(packet_t) == 17);
}
