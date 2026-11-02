/**
 * @file src/pen_barrel_roll.h
 * @brief pen barrel-roll extension.
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <moonlight-common-c/src/Input.h>

namespace input::pen_wire {
  constexpr std::uint32_t magic = 0x5500000B;  ///< Extended pen packet, negotiated by penBarrelRoll:1.
  constexpr std::uint16_t unknown = 0xffff;  ///< Missing barrel roll or azimuth.
  static_assert(magic != SS_PEN_MAGIC && magic != SS_TOUCH_MAGIC);
#ifdef SS_TOUCHPAD_MAGIC
  static_assert(magic != SS_TOUCHPAD_MAGIC);
#endif
#ifdef SS_TOUCHPAD_FRAME_MAGIC
  static_assert(magic != SS_TOUCHPAD_FRAME_MAGIC);
#endif

  /** @brief Detect working roll hardware before replacing the azimuth fallback. */
  class roll_probe_t {
  public:
    /** @brief Forget hardware observations at a protocol or session boundary. */
    void reset() {
      first_ = unknown;
      varied_ = false;
    }

    /**
     * @brief Resolve roll before batching so intermediate hardware changes are preserved.
     * @param roll Decoded barrel roll, or unknown.
     * @param azimuth Decoded pen-tip orientation, or unknown.
     * @return Independent roll once it varies; otherwise pen-tip orientation.
     */
    std::uint16_t select(std::uint16_t roll, std::uint16_t azimuth) {
      if (roll == unknown) {
        return azimuth;
      }
      if (first_ == unknown) {
        first_ = roll;
      }
      varied_ |= roll != first_;
      return varied_ ? roll : azimuth;
    }

  private:
    std::uint16_t first_ = unknown;  ///< First known hardware roll in this session.
    bool varied_ = false;  ///< Whether hardware has reported a change.
  };

#pragma pack(push, 1)

  /** @brief Legacy pen prefix followed by little-endian barrel roll and reserved bytes. */
  struct packet_t {
    SS_PEN_PACKET pen;  ///< Unmodified standard pen packet prefix.
    std::uint16_t barrelRoll;  ///< Independent roll in degrees, or unknown.
    std::uint16_t reserved;  ///< Reserved by the Foundation protocol.
  };

#pragma pack(pop)
  static_assert(sizeof(SS_PEN_PACKET) == 36);
  static_assert(offsetof(packet_t, barrelRoll) == 36);
  static_assert(sizeof(packet_t) == 40);

  /**
   * @brief Decode a little-endian pen angle while preserving its unknown sentinel.
   * @param value Wire angle.
   * @return Degrees in [0, 360), or unknown.
   */
  constexpr std::uint16_t decode_roll(std::uint16_t value) {
    if constexpr (std::endian::native == std::endian::big) {
      value = std::byteswap(value);
    }
    return value == unknown ? unknown : value % 360;
  }

  /**
   * @brief Require the exact extension layout, including its big-endian header size.
   * @param available Bytes received.
   * @param network_size Header size in network byte order.
   * @return Whether both lengths describe the 40-byte extension.
   */
  constexpr bool valid_size(std::size_t available, std::uint32_t network_size) {
    if constexpr (std::endian::native == std::endian::little) {
      network_size = std::byteswap(network_size);
    }
    return available == sizeof(packet_t) && network_size == sizeof(packet_t) - sizeof(network_size);
  }
}  // namespace input::pen_wire
