# Preview validation, 2026-10-05

This is a preview, not a stability guarantee across hardware or networks.
Measurements below use a real Windows virtual desktop and scrolling project
documents, a wired LAN, and a native 2560x1440 / 165 Hz client panel.
The existing development display driver was used while the packaged official
driver's Windows publisher confirmation remained pending.

## Verified behavior

- One PyroScreen.exe with send/receive modes: LAN discovery, manual IPv4, first pairing,
  remembered pairing, forgotten-host re-pairing, TLS control and AES-GCM video.
- Wrong pairing code, changed/tampered encrypted datagram and replay rejection.
- Actual GUI 120 -> 60 -> 120 mode changes retain one control connection.
- Manual disconnect/reconnect, tray hide/show, clean child-process shutdown.
- Client-process restart automatically reconnects while retaining the virtual
  display; host application startup automatically connects the remembered client.
- Local cursor is initially hidden, shown after programmatic local position
  movement, then hidden after two seconds idle. This is an automated position
  test, not a physical mouse hardware test.
- Source cursor movement does not crash the GPU compositor; source position is
  restored after the test.
- Clean build from pinned, unchanged upstream commits; static MSVC runtime in
  shipped native components. Source and release files exclude saved credentials.
- Frontend and test tools compile from the extracted source ZIP. Every listed
  release-file SHA256 was checked, including packaged driver and license files.
- The same application was run in both roles from its release directories; remembered pairing
  and native 2560x1440 startup succeeded without development runtime scripts.

## Workload correction

The earlier scrolling harness used a fixed 8192-row backing page. Shorter
project documents eventually scrolled into blank space. Those historical runs
therefore include low-content intervals and do not prove sustained unique
content throughput. The harness now uses actual document height and UTF-8 text,
keeps its viewport over real project content, and reruns the measurements.
The corrected AMD direct-render 2K/165 run measured 163.03 FPS over 174.767 steady
seconds, rolling P50 median 17.91 ms and rolling P95 median 20.93 ms. Maximum
presentation gap was43.19 ms (four sampled windows above20 ms), so occasional
stutters remain. Clock uncertainty was1-2 ms. This replaces neither physical
LCD response measurement nor validation on other machines.

## Performance evidence and limits

A three-minute 2K/120 run measured 119.60 actual presented updates per second.
The median of reported rolling latency medians was 14.94 ms; median rolling P95
was 18.94 ms. These are summaries of windows, not pooled frame quantiles.
Clock uncertainty was 1–2 ms. There were 95 incomplete codec packets, 10 frames
dropped in favor of fresher frames, and a 165.19 ms worst presentation gap.
The long run therefore does not establish zero stutters.

The later 40-second unprotected window measured 119.97 FPS, rolling median
latency median 17.29 ms and worst presentation gap 16.78 ms. Optional packet
protection did not establish an improvement (119.55 FPS, 17.93 ms, 51.22 ms worst
gap); it remains off by default. Short sequential comparisons are affected by
machine load and are not proof of causation.

The final release-directory 25-second steady window measured 120.02 FPS, median
reported rolling median latency 18.81 ms, median reported rolling P95 23.92 ms,
and a 16.14 ms worst presentation gap. This short run had zero incomplete codec
packets or freshness drops after the excluded five-second startup interval.

Earlier completed four-mode tests measured 60.03, 120.04 and 164.53 FPS at the
corresponding 2K settings. A 240 request was limited to a 165 Hz virtual display
and 165 Hz physical panel, measuring 164.84 FPS. 240 displayed FPS is unverified.

The final-code rerun measured 59.93 / 120.01 / 163.95 / 157.16 FPS for requests
60 / 120 / 165 / 240, with one control connection across all four modes.
The 240 request still used 165 Hz displays. Variation between runs is retained
in this report rather than selecting only the best measurements.

Latency measures source desktop update to DXGI display feedback with clock
uncertainty. It does not include LCD response time. Empty or expired sample
windows display --; periodic calibration no longer reports empty windows as 0.

## Unified application checks

Send -> receive stops the sender and releases its virtual display. Receive
mode listens for LAN connections. Closing and reopening restores that mode;
switching back to send automatically reconnects the remembered laptop.
Single-instance activation preserves the selected role. These lifecycle checks
and actual GUI switches 60 -> 120 -> 165 -> 240 -> 120 passed sequentially.
A separate overlapping test launch interfered with an earlier lifecycle run;
that attempt was excluded and rerun in order.

In the unified GUI run, the reported one-second presentation rates were
59.97 / 119.99 / 165.01 / 163.99 FPS for requests 60 / 120 / 165 / 240.
Corresponding rolling P50 latencies were 27.06 / 20.92 / 19.93 / 20.93 ms,
with a 1–2 ms calibration bound. Each mode used a scrolling document and was
sampled after eight seconds; these short windows are not long-term stability
or pooled frame quantiles. All four modes retained one TLS control connection.
The 240 request used a 165 Hz virtual display and physical panel.

The final ordinary-user check passed in one process: receiving, sending,
returning to receiving, releasing the elevated display helper, and reconnecting.
At 2560x1440 / 120 the reported one-second rate was 119.97 FPS and rolling
P50 latency 17.11 ms with a 1 ms clock bound. Only the display helper requests
UAC; the interface and video engines do not request elevation. These are short
functional windows and do not replace sustained performance checks.

Runtime files now use a unique per-process session directory. Both video
engines receive that exact directory from their controller, so calibration
and measurements cannot accidentally refer to an earlier launch. Local status
reads are restricted to the current Windows SID and do not return pairing
secrets. Corrupt or unreadable configuration fails visibly rather than silently
overwriting saved pairing state.

## Image evidence

A matched static project document was compared at 2560x1440. The observed
third-party RTSS overlay, injected after the source document's own snapshot,
and bottom HUD area were excluded. Mean RGB absolute error was 2.42/255 and
PSNR 30.30 dB. White medians were source (255,255,255), receiver (253,255,253);
dark text medians were source (15,20,26), receiver (16,19,23).
This supports no obvious limited-range conversion in this sample; lossy
compression errors remain. It is not comprehensive color or content-quality
acceptance. No brightness, contrast or gamma gain was introduced.

## Remaining release gates

- Complete packaged official-driver installation with native Windows approval.
  No Root/TrustedPublisher certificate import or test-signing changes.
- Exercise 1080p, 1600x900 and native-resolution changes on that driver, verify
  actual Windows mode and retained control connection, and exercise rollback.
- Clean-machine installation, other GPU vendors and longer sustained sessions.
- Further investigate rare long frame gaps before calling this a stable release.

Local raw results stay in results/ and are intentionally excluded from source
and release ZIPs; they can include private device names, IPs and screenshots.

## Encrypted segmentation experiment

A batch implementation independently encrypts/authenticates each1472-byte
segment, including padded tails. Round-trip, reverse-order receive, tamper
and replay checks passed. On the corrected real-document workload, however,
segmentation measured127.69 FPS over174.695 steady seconds, rolling P50median
19.93ms and P95median23.92ms, maxgap44.25ms. The same workload with regular IOCP
measured163.03FPS. Segmentation is not adopted and remains disabled by default.
No global NIC or overlay settings were changed. The sender's historical wire
byte counter omitted the32-byte GCM envelope in the individual-send path;
source now counts the sealed wire length. Timing conclusions do not depend
on the earlier bitrate undercount.
