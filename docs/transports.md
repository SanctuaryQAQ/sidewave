# Transport paths and latency

## Current implementation

- Video: UDP on port 55165, with optional AES-256-GCM encryption. The new
  sender defaults to plaintext video for LAN latency testing. Frames are fragmented
  into MTU-sized datagrams and optionally protected with one XOR parity
  fragment per codec packet. This is the project's own framing, not RTP.
- Control: TLS/TCP on port 55166. Discovery uses UDP 55167.
- The sender negotiates the video mode over the authenticated control
  connection. An older receiver must be updated before plaintext is selected.
  The GUI's Video encryption checkbox applies without reconnecting control.
- Both modes use 1,412-byte fragment payloads. The encrypted envelope adds
  32 bytes, while plaintext adds a 16-byte session identifier and sequence.
  Full UDP payloads are therefore 1,472 and 1,456 bytes respectively. Plaintext
  skips CNG key creation and all per-packet AES operations. Its sequence window
  filters duplicate/old packets but provides no cryptographic authentication.
- The sender uses bounded frame handoff and overlapped Winsock IOCP sends.
- D3D11/Vulkan share GPU textures and fences on each computer. The sender
  reads back the compressed bitstream, not a complete uncompressed video
  frame. The receiver decodes into shared GPU planes for D3D11 presentation.

## Shared memory

Windows named shared memory is local interprocess communication. It cannot
replace a network transport between two physical computers. Within each
video process, the capture/codec/network threads already share memory and
exchange bounded buffers. Adding another shared-memory IPC stage is not an
end-to-end zero-copy network implementation. See [Microsoft IPC guidance](https://learn.microsoft.com/en-us/windows/win32/ipc/interprocess-communications).

## DPDK with UDP

DPDK could provide a different packet I/O backend while retaining UDP and
this project's encryption/framing. It requires supported hardware, a suitable
Windows PMD/driver combination, and separate deployment and performance
validation. It does not remove codec, physical link serialization, GPU
synchronization, or display presentation time. A lower network-stack cost
must not be presented as a measured end-to-end latency improvement.

The current upstream Realtek R8169 PMD lists RTL8168 and RTL8125 hardware,
but its feature declaration lists Linux and does not declare Windows support.
It is therefore not a supported drop-in backend for Windows systems using
those adapters. This is different from saying that DPDK never supports
Realtek hardware. See [Realtek PMD](https://doc.dpdk.org/guides/nics/r8169.html),
[feature declaration](https://github.com/DPDK/dpdk/blob/main/doc/guides/nics/features/r8169.ini),
and [Windows setup requirements](https://doc.dpdk.org/guides/windows_gsg/run_apps.html).

No DPDK drivers or NIC bindings are installed by this application. Its
portable preview uses the normal Windows network stack.
