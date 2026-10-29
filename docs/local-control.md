# Local control API

PyroScreen exposes an explicit named-pipe API scoped to the current Windows
user. It does not simulate input, capture the screen, or use accessibility
automation. `INFO` reports application/engine state. `CTRL` accepts a bounded
JSON request and returns a command ID; `INFO` then reports its queued, running,
completed or failed state. Acceptance is distinct from completion.

## Command line

These commands target the already running application and never open a
PowerShell console. Use a result directory writable by the current user.

```powershell
.\PyroScreen.exe --status --result "$env:TEMP\pyroscreen-status.json"
.\PyroScreen.exe --apply-current --result "$env:TEMP\pyroscreen-apply.json"
.\PyroScreen.exe --stop-video --result "$env:TEMP\pyroscreen-stop.json"
.\PyroScreen.exe --control "settings-request.json" --result "$env:TEMP\pyroscreen-change.json"
```

Example `settings-request.json`:

```json
{
  "operation": "apply",
  "width": 2560,
  "height": 1440,
  "fps": 165,
  "budgetMbps": 900,
  "videoEncryption": false,
  "allowTearing": true,
  "packetProtection": true
}
```

Omitted settings keep their current values. `width: 0, height: 0` selects the
receiver's native resolution. Resolutions, FPS and budgets must match the UI's
presets; numeric fields are integers and switches are JSON booleans.
`apply` keeps the control connection and virtual display lease. Unchanged
parameters leave the active video engines running. `stop-video` stops only
video; `disconnect` also releases the display and connection. `--quit` requests
application exit. Display operations require sender mode.

Exit code 0 means the operation completed successfully, except `--quit`, whose
reply acknowledges the exit request. Exit code 1 indicates rejection/failure.
`--status` returns the INFO object; other completed commands return
`passed`, `id` and `state`. INFO includes `controlApi`, `controlId`,
`controlState`, `controlError`, `busy`, `lastStatus`, connection/display IDs,
engine PID and current telemetry. API operations update visible controls as
well as persisted configuration.

INFO also reports `senderPid`, `senderExitDiagnostic`, `videoRequested`,
`senderVideoRecoveries` and `senderRecoveryAttempts`. A live TLS connection is
distinct from `streaming`: an unexpectedly exited sender triggers at most
three video recovery attempts, preserving the connection and display lease.
Explicit Stop/Disconnect or a closed receiver window cancels recovery. The sender's live
statistics include `udp_buffer_pressure_retries_total` for temporary UDP
submission pressure.

`allowTearing` selects supported vsync-off presentation and can cause visible
tearing. The mode is capability-negotiated, and native hardware support is
checked. The ordinary-user HUD has no validated full-chain display latency for
this mode and shows `--`.
New profiles default to false.

## Wire format

- Pipe: `PyroScreen-<current Windows user SID>`.
  Its ACL grants the owning SID access; the provided CLI connects locally.
- Status: four ASCII bytes `INFO`.
- Control: four ASCII bytes `CTRL`, signed little-endian 32-bit byte count,
  then UTF-8 JSON. Request size must be 2–16,384 bytes.
- Both replies use a little-endian byte count followed by UTF-8 JSON.
- Allowed operations: `apply`, `stop-video`, `disconnect`, `quit`.
  Unknown operations/fields and commands issued while busy are rejected.
- This interface does not accept credentials, raw video keys or arbitrary
  executable/shell commands. Network pairing remains in the TLS control path.

## Verification

The application was operated exclusively through this interface after its
installation. Tests confirmed idempotent Apply, stop/start preserving the same
connection and virtual display, rejection of invalid FPS and unknown fields,
and continued streaming after rejected commands. These checks used no GUI input.
