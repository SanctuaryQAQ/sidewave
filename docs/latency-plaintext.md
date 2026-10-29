# Plaintext video latency experiment — 2026-10-06

Video encryption is optional and disabled by default. Plaintext bypasses AES
key creation and per-packet encrypt/decrypt calls, and removes the 16-byte GCM
tag. Pairing, clock calibration and commands still use the TLS control channel.
The application negotiates the mode before starting either video engine.

Both computers were updated automatically and their application, XAML and
sender/receiver executable hashes matched. Native telemetry on both ends
confirmed `encrypted_video=false` after the tests. Switching the video mode
kept the same control connection and virtual display.

## Real extended-desktop measurements

2560×1440, 4:4:4, 165 Hz effective limit, 900 Mbps requested codec ceiling,
FEC enabled, integrated-GPU presentation, waitable presentation disabled.
Main monitor scaling was 125%; the extended monitor was 150%.
The source was a real project document scrolling on the virtual desktop,
rendered with per-monitor DPI awareness. Source throughput was 161.6–163.1
FPS over complete runs including startup and shutdown; these runs therefore
do not demonstrate a perfectly constant 165 FPS source or receiver.

| Video / sender | Receiver measurement | Displayed FPS | Median of rolling P50 | Median of rolling P95 | Maximum display gap |
|---|---:|---:|---:|---:|---:|
| Plaintext / IOCP, first run | 89.39 s | 163.10 | 19.94 ms | 21.93 ms | 42.89 ms |
| AES-GCM / IOCP, same software | 59.27 s | 163.25 | 17.94 ms | 19.91 ms | 47.35 ms |
| Plaintext / RIO | 89.42 s | 163.79 | 17.93 ms | 22.93 ms | 51.87 ms |
| Plaintext / IOCP, final run | 90.41 s | 163.64 | 19.94 ms | 21.92 ms | 43.81 ms |

Displayed FPS uses the presentation counter delta divided by receiver elapsed
time, with duplicate telemetry samples removed. The latency columns summarize
per-second rolling-window percentiles; they are not pooled run percentiles.
The endpoint is the source desktop update to DXGI display feedback. Physical
LCD response is excluded. Clock calibration bounds reached ±2 ms.
Normal desktop activity varied between runs; packaging also ran briefly
during the final measurement. These are practical software measurements,
not a controlled proof of an encryption-specific latency reduction.

Removing encryption did not produce a repeatable end-to-end improvement in
these runs. It remains disabled as requested. RIO did not improve the latency
tail or maximum gap, and its controller also settled at a lower data rate.
The final default is normal overlapped Winsock IOCP; RIO remains an optional
native experimental backend. Stable continuous 165 FPS and removal of the
occasional roughly 44 ms display gap remain unresolved.

## Transport checks

- Encrypted and plaintext round trips, reverse-order batches, duplicate rejection
  and plaintext session-identifier filtering passed. Tamper rejection was checked
  only for the encrypted mode; plaintext has no cryptographic authentication.
- RIO sent 4,097 datagrams in each mode, crossing the former 1,024-packet boundary.
  All datagrams arrived with matching payloads; invalid and receive-error counts
  were zero. These loopback tests are not end-to-end latency measurements.
- A full encrypted datagram is 1,472 bytes; plaintext is 1,456 bytes. FEC and
  line-budget estimates use the negotiated envelope size. No RTP, DPDK driver
  installation or NIC rebinding was introduced.
