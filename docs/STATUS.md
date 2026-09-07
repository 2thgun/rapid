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
- Current commit `85ff713` passed all five Pi Debug CTest targets: network,
  archive, runtime/recorder, log status and protocol. Its Release build passed
  the four then-release-safe targets. The current protocol test uses explicit
  failures so subsequent Release runs can include all five targets.
  Network checks cover UDP, HTTP, WebSocket history and disconnect finalization.
- A real journal event advanced the native HTTP status with warning severity.
- Dashboard services started at 8.500/8.503 seconds into userspace after removing
  their network-online dependency. Overall boot remains about 25 seconds.

## Current change

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
  [GitHub Actions for 47860ec](https://github.com/2thgun/rapid/actions/runs/34064690027).
- Four-simulator live acceptance and immediate lap publication remain open.
