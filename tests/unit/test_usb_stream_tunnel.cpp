/**
 * @file tests/unit/test_usb_stream_tunnel.cpp
 * @brief USB framing, backpressure and macOS usbmux relay integration tests.
 */
#include "../tests_common.h"
#include "src/usb_stream_tunnel.h"

#include <future>

#ifdef __APPLE__
  #include <boost/asio.hpp>
  #include <boost/asio/local/stream_protocol.hpp>
  #include <filesystem>
  #include <nlohmann/json.hpp>
  #include <unistd.h>
#endif

using namespace std::chrono_literals;

/** @brief Datagram framing preserves binary encryption prefixes and rejects invalid lengths. */
TEST(UsbStreamTunnel, DatagramFraming) {
  std::vector<char> batch;
  EXPECT_TRUE(usb_stream_tunnel::append_video_datagram(batch, std::string_view("\0\xff", 2), "payload"));
  ASSERT_EQ(batch.size(), 11);
  EXPECT_EQ(static_cast<unsigned char>(batch[0]), 0);
  EXPECT_EQ(static_cast<unsigned char>(batch[1]), 9);
  EXPECT_EQ(std::string(batch.begin() + 2, batch.end()), std::string("\0\xffpayload", 9));
  const auto original = batch;
  EXPECT_FALSE(usb_stream_tunnel::append_video_datagram(batch, {}, {}));
  EXPECT_FALSE(usb_stream_tunnel::append_video_datagram(batch, {}, std::string(65508, 'x')));
  EXPECT_FALSE(usb_stream_tunnel::append_video_datagram(batch, "x", std::string(65507, 'x')));
  EXPECT_EQ(batch, original);
}

/** @brief Overflow closes the queue; reader and shutdown notifications release blocked threads. */
TEST(UsbStreamTunnel, BackpressureAndReaderLifecycle) {
  usb_stream_tunnel::video_channel channel;
  EXPECT_FALSE(channel.wait_for_reader(1ms));
  auto ready = std::async(std::launch::async, [&] {
    return channel.wait_for_reader(3s);
  });
  channel.reader_started();
  ASSERT_EQ(ready.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(ready.get());
  ASSERT_TRUE(channel.enqueue({'a', 'b'}));
  std::vector<char> out;
  ASSERT_TRUE(channel.next_batch(out));
  EXPECT_EQ(out, (std::vector<char> {'a', 'b'}));
  ASSERT_TRUE(channel.enqueue(std::vector<char>(2 * 1024 * 1024, 'x')));
  EXPECT_FALSE(channel.enqueue({'x'}));
  EXPECT_FALSE(channel.active());
  EXPECT_FALSE(channel.next_batch(out));
  EXPECT_FALSE(channel.wait_for_reader(1ms));
  EXPECT_FALSE(usb_stream_tunnel::claim_video("not-a-token"));
  EXPECT_FALSE(usb_stream_tunnel::claim_video("00000000-0000-0000-0000-000000000000"));
}

#ifdef __APPLE__
namespace {
  namespace asio = boost::asio;
  using mux_socket = asio::local::stream_protocol::socket;
  using tcp = asio::ip::tcp;
  using udp = asio::ip::udp;
  using json = nlohmann::json;

  /** @brief Read or write exactly one management message with a bounded test deadline. */
  template<class Socket>
  void transfer(Socket &socket, char *bytes, std::size_t size, bool writing) {
    socket.non_blocking(true);
    const auto until = std::chrono::steady_clock::now() + 4s;
    while (size) {
      boost::system::error_code error;
      const auto count = writing ? socket.write_some(asio::buffer(bytes, size), error) : socket.read_some(asio::buffer(bytes, size), error);
      if (error == asio::error::would_block || error == asio::error::try_again) {
        if (std::chrono::steady_clock::now() > until) {
          throw std::runtime_error("USB test transfer timed out");
        }
        std::this_thread::sleep_for(1ms);
        continue;
      }
      if (error || count == 0) {
        throw std::runtime_error("USB test transfer closed");
      }
      size -= count;
      bytes += count;
    }
  }

  /** @brief Send a binary-safe test message through a relay. */
  template<class Socket>
  void write(Socket &socket, std::string bytes) {
    transfer(socket, bytes.data(), bytes.size(), true);
  }

  /** @brief Read a known-size message through a relay. */
  template<class Socket>
  std::string read(Socket &socket, std::size_t size) {
    std::string bytes(size, '\0');
    transfer(socket, bytes.data(), bytes.size(), false);
    return bytes;
  }

  /** @brief Receive a length-prefixed VoidLink JSON header. */
  json read_header(mux_socket &socket) {
    const auto header = read(socket, 2);
    const auto size = (std::size_t(static_cast<unsigned char>(header[0])) << 8) | static_cast<unsigned char>(header[1]);
    if (size == 0 || size > 8192) {
      throw std::runtime_error("Invalid USB test JSON header");
    }
    return json::parse(read(socket, size));
  }

  /** @brief Local usbmux daemon returning one USB device and one ignored network device. */
  class fake_mux {
  public:
    asio::io_context io;  ///< Context owning the test Unix endpoint.
    std::string path;  ///< Unique short Unix socket path.
    asio::local::stream_protocol::acceptor acceptor;  ///< Fake usbmux management listener.
    std::mutex mutex;  ///< Protects completed Connect requests.
    std::condition_variable changed;  ///< Signals a new tunnel connection.
    std::deque<std::shared_ptr<mux_socket>> connections;  ///< Connections handed to the simulated iPad.
    std::atomic<int> invalid_connections {0};  ///< Unexpected device or port requests.
    std::jthread worker;  ///< Management accept loop.

    /** @brief Start a fake daemon without accessing the real usbmux service. */
    fake_mux():
        path("/tmp/canvas-usb-" + std::to_string(getpid()) + "-" + std::to_string(counter++)),
        acceptor(io, asio::local::stream_protocol::endpoint(path)),
        worker([this](std::stop_token stop) {
          serve(stop);
        }) {}

    /** @brief Join the server and remove its Unix socket. */
    ~fake_mux() {
      worker.request_stop();
      worker.join();
      std::filesystem::remove(path);
    }

    /** @brief Wait for the next successful mux Connect. */
    std::shared_ptr<mux_socket> take() {
      std::unique_lock lock {mutex};
      if (!changed.wait_for(lock, 4s, [&] {
            return !connections.empty();
          })) {
        throw std::runtime_error("No USB test connection");
      }
      auto socket = connections.front();
      connections.pop_front();
      return socket;
    }

  private:
    inline static std::atomic<unsigned> counter {0};  ///< Path discriminator for multiple tests.

    /** @brief Encode a little-endian usbmux plist response. */
    void reply(mux_socket &socket, std::string payload) {
      std::string header(16, '\0');
      const auto put = [&](int offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) {
          header[offset + i] = static_cast<char>(value >> (8 * i));
        }
      };
      put(0, payload.size() + 16);
      put(4, 1);
      put(8, 8);
      put(12, 1);
      write(socket, header + payload);
    }

    /** @brief Accept ListDevices and Connect requests, retaining each raw data connection. */
    void serve(std::stop_token stop) {
      acceptor.non_blocking(true);
      while (!stop.stop_requested()) {
        auto socket = std::make_shared<mux_socket>(io);
        boost::system::error_code error;
        acceptor.accept(*socket, error);
        if (error) {
          std::this_thread::sleep_for(2ms);
          continue;
        }
        try {
          const auto header = read(*socket, 16);
          uint32_t size = 0;
          for (int i = 0; i < 4; ++i) {
            size |= uint32_t(static_cast<unsigned char>(header[i])) << (8 * i);
          }
          if (size < 16 || size > 8192) {
            continue;
          }
          const auto request = read(*socket, size - 16);
          if (request.find("<string>ListDevices</string>") != std::string::npos) {
            reply(*socket,
                  "<plist><dict><key>DeviceList</key><array>"
                  "<dict><key>DeviceID</key><integer>1</integer><key>Properties</key><dict><key>ConnectionType</key><string>USB</string></dict></dict>"
                  "<dict><key>DeviceID</key><integer>2</integer><key>Properties</key><dict><key>ConnectionType</key><string>Network</string></dict></dict>"
                  "</array></dict></plist>");
          } else if (request.find("<string>Connect</string>") != std::string::npos) {
            if (request.find("<key>DeviceID</key><integer>1</integer>") == std::string::npos || request.find("<key>PortNumber</key><integer>31139</integer>") == std::string::npos) {
              ++invalid_connections;
            }
            reply(*socket, "<plist><dict><key>Number</key><integer>0</integer></dict></plist>");
            std::lock_guard lock {mutex};
            connections.push_back(std::move(socket));
            changed.notify_all();
          }
        } catch (const std::exception &) {
        }
      }
    }
  };
}  // namespace

/** @brief Exercise the unmodified VoidLink wire protocol through a fake macOS usbmux daemon. */
TEST(UsbStreamTunnel, MacOsOfferTcpUdpAndDirectVideo) {
  fake_mux mux;
  asio::io_context io;
  tcp::acceptor http(io, {asio::ip::address_v4::loopback(), 0});
  udp::socket control(io, {asio::ip::address_v4::loopback(), 0});
  const usb_stream_tunnel::ports_t ports {http.local_endpoint().port(), 47984, 48010, 47998, control.local_endpoint().port(), 48000, 48001};
  std::atomic<unsigned> ids {0};
  std::jthread host([&](std::stop_token stop) {
    usb_stream_tunnel::run(stop, "00000000-0000-0000-0000-000000000001", "Canvas USB Test", "127.0.0.1", ports, [&] {
      return "00000000-0000-0000-0000-" + std::string(11, '0') + std::to_string(++ids);
    },
                           mux.path);
  });
  auto offer_socket = mux.take();
  const auto offer = read_header(*offer_socket);
  ASSERT_EQ(offer.at("type"), "offer");
  ASSERT_EQ(offer.at("ports").size(), 7);
  ASSERT_TRUE(offer.at("directVideo").get<bool>());
  ASSERT_EQ(offer.at("hostName"), "Canvas USB Test");

  write(*offer_socket, "OPEN 1 0\n");
  auto http_usb = mux.take();
  const auto http_header = read_header(*http_usb);
  ASSERT_EQ(http_header.at("id"), 1);
  ASSERT_EQ(http_header.at("session"), offer.at("session"));
  tcp::socket http_peer(io);
  http.accept(http_peer);
  write(*http_usb, "GET /serverinfo HTTP/1.1\r\n\r\n");
  EXPECT_EQ(read(http_peer, 28), "GET /serverinfo HTTP/1.1\r\n\r\n");
  write(http_peer, "HTTP/1.1 200 OK\r\n\r\npaired");
  EXPECT_EQ(read(*http_usb, 25), "HTTP/1.1 200 OK\r\n\r\npaired");

  write(*offer_socket, "OPEN 2 4\n");
  auto control_usb = mux.take();
  EXPECT_EQ(read_header(*control_usb).at("id"), 2);
  write(*control_usb, std::string("\0\x04", 2) + "ctrl");
  std::array<char, 16> datagram;
  udp::endpoint peer;
  control.non_blocking(true);
  std::size_t received = 0;
  const auto until = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < until) {
    boost::system::error_code error;
    received = control.receive_from(asio::buffer(datagram), peer, 0, error);
    if (!error) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_EQ(received, 4);
  EXPECT_EQ(std::string(datagram.data(), received), "ctrl");
  control.send_to(asio::buffer("reply", 5), peer);
  EXPECT_EQ(read(*control_usb, 7), std::string("\0\x05", 2) + "reply");

  write(*offer_socket, "OPEN 3 7\n");
  auto video_usb = mux.take();
  const auto video_header = read_header(*video_usb);
  ASSERT_EQ(video_header.at("type"), "video");
  ASSERT_EQ(video_header.at("id"), 3);
  auto video = usb_stream_tunnel::claim_video(video_header.at("token").get<std::string>());
  ASSERT_NE(video, nullptr);
  EXPECT_FALSE(usb_stream_tunnel::claim_video(video_header.at("token").get<std::string>()));
  EXPECT_FALSE(video->wait_for_reader(1ms));
  write(*video_usb, "V");
  ASSERT_TRUE(video->wait_for_reader(3s));
  std::vector<char> batch;
  ASSERT_TRUE(usb_stream_tunnel::append_video_datagram(batch, "encrypted-prefix", "fec-payload"));
  const auto expected = std::string(batch.begin(), batch.end());
  ASSERT_TRUE(video->enqueue(std::move(batch)));
  EXPECT_EQ(read(*video_usb, expected.size()), expected);

  // Keep the original 8 KB Unix socket buffers to detect accidental polling-based throttling.
  video_usb->non_blocking(false);
  const auto burst_start = std::chrono::steady_clock::now();
  const std::vector<char> burst(1024 * 1024, 'v');
  ASSERT_TRUE(video->enqueue(burst));
  std::vector<char> received_burst(burst.size());
  asio::read(*video_usb, asio::buffer(received_burst));
  const auto burst_time = std::chrono::steady_clock::now() - burst_start;
  EXPECT_EQ(received_burst, burst);
  EXPECT_LT(burst_time, 500ms);
  RecordProperty("usb_burst_1mib_ms", std::chrono::duration_cast<std::chrono::milliseconds>(burst_time).count());

  // A stalled client must not prevent offer teardown from cancelling a blocked video writer.
  ASSERT_TRUE(video->enqueue(burst));
  std::this_thread::sleep_for(20ms);
  const auto teardown_start = std::chrono::steady_clock::now();
  boost::system::error_code ignored;
  offer_socket->close(ignored);
  const auto closing = std::chrono::steady_clock::now() + 3s;
  while (video->active() && std::chrono::steady_clock::now() < closing) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_FALSE(video->active());
  EXPECT_EQ(mux.invalid_connections.load(), 0);
  host.request_stop();
  host.join();
  EXPECT_LT(std::chrono::steady_clock::now() - teardown_start, 1s);
}
#endif
