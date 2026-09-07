# Status — 2026-09-07

Migration update: 2026-09-07. The Pi now runs the validated native C++ dashboard
and recorder. Isolated Debug and Release checks passed before cutover; live
simulator acceptance remains open.

## Last verified

- Pi `rapid.service`, `rapid-display.service`, and `rapid-log-status.service` are active
  with zero restarts after cutover. `rapid.service` runs C++ `rapid-pi`.
- The deployed dashboard has Drive, Timing, Vehicle, Tyres, and Graphs touch pages.
  Graphs retain 30 seconds of pedal and G-force history in the dashboard browser.
- `\\rapid\Telemetry` is the working authenticated SMB location for finalized bundles.
- Native Windows companion source builds and passes its synthetic LD self-test.
- Deployed native revision `3c6aa8b` passed all five Pi Release CTest targets:
  network, archive, runtime/recorder, log status and protocol. Binary and dashboard
  hashes match the tested staging build; hosted Debug/Release tests also passed.
  Network checks cover UDP, HTTP, WebSocket history and disconnect finalization.
- A real journal event advanced the native HTTP status with warning severity.
- Boot before cleanup measured 25.904 seconds. Removed the obsolete seven-second
  rc.local sleep and failed framebuffer-copy launch; cold-boot improvement is unmeasured.
- Kiosk revision `7686e12` publishes its X11 activation environment. Both desktop
  portal services are now active after previously failing to open the display.

## Current change

Authenticated v4 is implemented in the Windows sender and Pi receiver. The final
isolated Pi Release build passed all six CTests, including actual Windows BCrypt
fixtures, validity/loss regression checks and replay persistence. A Windows-to-Pi
UDP check on separate ports verified two samples, one rejected replay and session
finalization. Hosted CI for this change is pending.
Production remains configured for v3; live v4 simulator acceptance is pending.
See [v4 setup and protocol](TELEMETRY_V4.md) before pairing keys.

Display output now goes only to the journal, preventing Xorg and Chromium output
from drawing over the dashboard. A three-second “Log updated” badge is driven by
the native C++ monitor. Full messages remain in the persistent journal, limited
to 100 MB and 14 days.

## Known limits

- Lap files are produced when a session bundle finalizes, not after each completed lap.
- Fallback sectors are distance thirds where canonical sector loops are unavailable.
- Native Pi/archive implementation is present. Live simulator and archive
  acceptance remain pending.
- Linux native tests and the Windows companion build/self-test passed in
  [GitHub Actions for 3c6aa8b](https://github.com/2thgun/rapid/actions/runs/34132243838).
- Four-simulator live acceptance and immediate lap publication remain open.
