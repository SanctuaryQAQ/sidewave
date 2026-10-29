# PyroScreen acceptance

Status: development; not a stable release until the checks below pass.

- [x] Prototype: real desktop capture, GPU 4:4:4 SDR presentation.
- [x] Prototype: 60/120/165/240 selections; report physical and virtual limits.
- [x] Prototype: hide local cursor until movement, hide after two seconds.
- [x] One portable executable with send/receive modes, no WinRM runtime dependency.
- [x] LAN discovery, manual IPv4, pairing, remembered device identity and reconnect.
- [x] Authenticated encrypted control and video, bounded inputs and queues.
- [x] Frame-rate changes with one persistent control session.
- [ ] Resolution changes on the packaged official driver with one persistent session.
- [x] Modern interface, role icons, tray, shutdown, firewall and diagnostics.
- [ ] Signed-driver installation without development certificates or test signing.
- [x] Real two-machine 2K acceptance, four frame-rate modes, reconnect, long run.
- [x] Client restart recovery keeps the virtual display, startup remembers pairing.
- [x] Local cursor hidden initially, movement shows it, two seconds idle hides it.
- [x] Source package: licenses, portable build, no credentials or machine paths.

Performance is measured from actual frames. Requested 240 FPS does not imply
240 displayed frames on a 165 Hz laptop. Latency is desktop update to DXGI display
feedback with a clock uncertainty; panel response needs external measurement.
