/**
 * @file src/usb_stream_tunnel.h
 * @brief USB tunnel protocol and bounded direct-video channel.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace usb_stream_tunnel {

  /** @brief HTTP, HTTPS, RTSP, legacy video, control, audio and microphone ports, in protocol order. */
  using ports_t = std::array<std::uint16_t, 7>;

  /** @brief HTTPS header carrying the single-use video-channel token. */
  inline constexpr std::string_view video_launch_header = "X-SS-USB-Direct-Video";

  /** @brief Bounded packet queue shared by an authenticated stream and its USB writer. */
  class video_channel {
  public:
    /** @brief Queue a framed batch; close the channel if the queue limit would be exceeded. */
    bool enqueue(std::vector<char> batch);
    /** @brief Wait briefly for a batch, returning false when closed or still empty. */
    bool next_batch(std::vector<char> &batch);
    /** @brief Return whether the channel remains usable. */
    bool active() const;
    /** @brief Wait for the client receive thread to signal readiness. */
    bool wait_for_reader(std::chrono::milliseconds timeout);
    /** @brief Signal that the client has started reading video. */
    void reader_started();
    /** @brief Close the channel, discard pending data and wake waiting threads. */
    void close();

    std::atomic_bool claimed {false};  ///< Set only after HTTPS consumes the token.

  private:
    mutable std::mutex mutex_;  ///< Protects channel state and pending batches.
    std::condition_variable ready_;  ///< Wakes the writer and reader-start waiters.
    std::deque<std::vector<char>> queue_;  ///< Framed video batches awaiting USB transmission.
    std::size_t queued_bytes_ = 0;  ///< Total buffered bytes subject to the queue limit.
    bool closed_ = false;  ///< Terminal state, including queue overflow.
    bool reader_started_ = false;  ///< Client receive thread sent its startup marker.
  };

  /** @brief Consume a published token once; return no channel if unknown, expired or closed. */
  std::shared_ptr<video_channel> claim_video(std::string_view token);

  /** @brief Append one encrypted/FEC datagram with its two-byte big-endian length. */
  bool append_video_datagram(std::vector<char> &batch, std::string_view prefix, std::string_view payload);

  /**
   * @brief Discover physical USB devices and relay only the configured GameStream services.
   * @param stop Cooperative shutdown signal.
   * @param host_id Persistent host UUID advertised to VoidLink.
   * @param host_name Friendly host name.
   * @param local_address Concrete local address for the existing GameStream listeners.
   * @param ports Mapped GameStream service ports, including the reserved microphone slot.
   * @param make_session Generates opaque UUIDs for offers and single-use video tokens.
   * @param mux_path macOS usbmux Unix socket; injectable for protocol integration tests.
   */
  void run(std::stop_token stop, std::string host_id, std::string host_name, std::string local_address, ports_t ports, std::function<std::string()> make_session, std::string mux_path = "/var/run/usbmuxd");

}  // namespace usb_stream_tunnel
