# Protocol v1

Discovery is an unauthenticated hint, never proof of device identity. Bounded
JSON UDP messages advertise a persistent random device GUID, display metadata
and name. Host uses the response source address, not an advertised redirect.

Every three seconds the host enumerates active IPv4 LAN interfaces, binds a
reply socket to each local address, and sends a query to that interface's
subnet broadcast address on UDP 55167. Saved IP addresses are not discovery
targets; they remain a fallback for authenticated reconnection. The refresh
button can additionally query the manually entered IP. Interface changes
recreate the corresponding socket. Devices with no reply for twelve seconds
are removed from the visible list. Multiple replies update one device GUID.
Discovery requires the receiver to be running, firewall access and a shared
broadcast domain. Guest Wi-Fi/client isolation or different VLANs can prevent
discovery even when addresses appear to be in the same subnet.

Control uses TLS 1.2 over TCP. Records are a four-byte network-order byte count
and UTF-8 JSON, limited to 32768 bytes and shallow nesting. TCP_NODELAY is enabled.
There are at most four pending TLS handlers, one authenticated video owner,
timeouts and pairing retry delays.

On first pair the client displays a random 80-bit Base32 code. Both ends hash
the code into the temporary authentication key. The authenticated transcript is:

    PyroScreen|1|hostID|clientID|hostNonce|clientNonce|actualCertificateSHA256

The client proves HMAC(key, "server|" + transcript) and the host proves
HMAC(key, "host|" + transcript). The certificate is the TLS peer certificate;
domain separation prevents reflection, random challenges prevent replay and
certificate binding prevents a first-pair TLS relay with a substituted certificate.
The client then generates a 256-bit remembered token, sent inside this TLS
session and encrypted at rest with CurrentUser DPAPI. Later sessions use that
token. The most recent client certificate is additionally pinned.

Authenticated operations: clock, calibrate, start, stats, stop, disconnect.
Every control session has its own connection GUID. Mode changes retain both
this TCP session and the host-owned software-device lease. Video resources are
recreated with a fresh 44-byte random secret; a failed mode may be rolled back
by the UI. No request permits commands, scripts, downloads or arbitrary paths.

Video secret: AES-256 key (32), nonce prefix (4), session ID (8).
Each UDP datagram:

| Bytes | Content |
| --- | --- |
| 0–7 | session ID |
| 8–15 | little-endian sequence number |
| 16… | GCM ciphertext: 28-byte codec fragment header + <=1412-byte payload |
| last 16 | GCM tag |

GCM IV is the 4-byte nonce prefix and 8-byte sequence. The outer 16-byte header
is authenticated associated data. The receiver checks peer IP, session ID,
authentication tag and an 8192-packet replay window before codec parsing.
Replay state only advances after successful tag validation. Sequence numbers
are scoped to one newly generated session key, not reused on mode changes.

The four-timestamp clock exchange is inside the authenticated control
session. Minimum observed round trip yields a clock-offset estimate and bound;
calibration renews every five seconds and expires after 60 seconds. Constant
cadence is negotiated using supportsSnapshotCadence / captureTimestampKind.
The current host negotiates desktop_update timestamps. The timestamp is the
actual desktop/pointer update, including time before capture; cached pixels
retain their original source timestamp. Snapshot timing remains a diagnostic
option and is not used for the normal full-chain HUD.

The receiver writes a guarded QPC/precise-UTC/source-clock marker immediately
before Present into a current-user memory mapped ring. A protected per-user
Windows task uses the pinned PresentMon 2.6 collector, filters the current
receiver PID and matches DXGI present events to those markers. LAT ends at the
reported system display event, including post-Present render/display waiting.
No fixed display offset is added. Each original source update contributes only
its first observed display; repeated cached content is excluded. Observer lag
is reported separately. Missing, stale or uncalibrated events produce --.
Present-return latency remains diagnostics only; neither endpoint measures
physical panel response.

This is a new protocol and has not had an independent security audit.

Optional UDP segmentation batches complete encrypted 1472-byte datagrams.
Each segment has its own sequence, GCM tag and authenticated header. Short
codec tails are padded inside GCM; packetBytes preserves meaningful length.
The IPv4 MTU remains1500 and the receiver applies the same replay checks.
Segmentation is an experimental local sender setting, off by default pending
sustained measurements. It does not send an unprotected multi-packet frame.

## Sender deadline budget

The requested Mbps remains an encoding ceiling. The sender also constrains
quality against measured transmit capacity without changing the capture target.
Once per statistics interval (about one second), the 90th percentile completed
frame transmit time in constant cadence mode is compared with the capture
period. Legacy desktop-update mode uses the median instead. Above 95% of that
period, the active codec budget shrinks by at most 10%, targeting 90% of the
period. Below 80%, the ceiling recovers by 2.5% per interval. It stays within the
requested budget and the 64 KiB codec floor; wire-overhead control still applies.
This is quality adaptation, not retransmission or a resolution/chroma change.
The 90th percentile generally excludes isolated stalls while retaining headroom
for slower frames. The udp_send_p50_ms field remains the median diagnostic;
udp_send_p95_ms separately records the slower tail.
`--no-send-deadline-control` disables it for controlled comparisons.

## Client cursor (experimental)

With `clientCursor` negotiated in `start` (receiver advertises
`supportsClientCursor`), the sender no longer composes the pointer into video
and passes `--client-cursor` to both native processes. A separate sender
socket sends sealed datagrams in the same session, key and sequence space
(the video and cursor threads serialize `Seal`), so the receiver applies the
same peer, tag and replay checks. Plaintext starts with magic `0x5057525A`:
kind (1 position, 2 shape chunk), visible, shape serial, hotspot x/y in output
pixels and the host UTC microsecond sample time. A 1 kHz host thread samples
`GetCursorInfo` and sends a position on change, else every 100 ms. Shapes come
from Desktop Duplication, at most 256x256, in 1380-byte chunks, resent every
second so a lost chunk heals. The receiver builds an `HCURSOR`, applies it in
`WM_SETCURSOR` and calls `SetCursorPos` only while its window is foreground.
Pointer-only desktop updates are not treated as new video sources.
`cursor_latency_*` covers host sample to receiver `SetCursorPos`; it excludes
hardware cursor scan-out and is not directly comparable with video LAT.
