# USB wired streaming on macOS

Connect the iPad or iPhone with a data-capable USB cable, unlock it, and trust the
Mac if prompted. Open VoidLink and select the Mac from its **USB** host list.
For the first connection, enter the client's pairing PIN in the host's web UI.
Pairing and streaming use the cable; the client's Wi-Fi is not required.

The host connects to the system usbmux service at `/var/run/usbmuxd`, considers
only devices with `ConnectionType = USB`, and opens VoidLink's listener at device
port 41849. No third-party usbmux library is needed. The host retries discovery
when the device or client app is temporarily unavailable.

## Protocol

The protocol is compatible with Foundation Sunshine's USB stream tunnel. Each
connection starts with a two-byte big-endian JSON length and a JSON header.
The initial header has `type: offer`, a session UUID, `hostId`, `hostName`,
`directVideo: true`, and seven mapped ports in this order:

1. GameStream HTTP, including initial pairing.
2. GameStream HTTPS, including pairing completion, apps, launch and resume.
3. RTSP.
4. Legacy video UDP port, unused by direct-video clients.
5. ENet control and input UDP.
6. Audio UDP.
7. Reserved microphone UDP slot. Its presence does not advertise microphone
   support; macOS microphone return is not implemented by this transport.

VoidLink sends `OPEN <id> <service-index>\n` on the offer connection. The host
opens the matching local GameStream service and a new USB connection, responding
with `type: channel`, the offer's session UUID and the requested channel ID.
TCP services carry raw bytes. UDP services use two-byte big-endian datagram
lengths and preserve datagram boundaries. Only configured GameStream services
can be opened; each relay has independent backpressure.

Video uses `OPEN <id> 7\n`. The host replies on a separate connection with
`type: video`, `session`, `id` and a single-use token. The paired client supplies
that token in the `X-SS-USB-Direct-Video` header on HTTPS launch/resume. RTSP
ANNOUNCE must request `x-ml-video.usbStreamVersion:1`; the host acknowledges it
with `X-SS-USB-Video-Version: 1`.

When common-c starts reading, VoidLink sends a single `V` byte on the video
connection. The host then sends the existing video datagrams, including
negotiated encryption prefixes and FEC, with two-byte big-endian lengths.
Direct video bypasses local video UDP forwarding and the video UDP ping.
On macOS, blocked reads and writes wait for Unix-socket readiness rather than
sleeping between attempts. The readiness wait checks cancellation periodically
without imposing an interval on ready sockets. The bounded video queue closes
the affected stream if the USB writer cannot keep up. Closing the offer or stopping the host closes its relays and video
channels. USB does not bypass GameStream pairing, certificate validation or
negotiated media encryption.

## Building and checking

The USB transport is built into the macOS host. Tests include packet framing,
queue limits, reader readiness, a simulated Unix-socket usbmux daemon, TCP and
UDP round trips, one-time video tokens, direct video, and offer disconnect
cleanup. Hardware throughput and cable reconnect behavior should also be checked
with a real VoidLink device.
