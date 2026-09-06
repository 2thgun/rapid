# Status — 2026-09-06

Migration update: 2026-09-06. Native Pi and archive implementations now build and
pass initial isolated tests on the Pi. The production service has not been switched.
The optimized build, extended upload test and latest compatibility fixes still
require verification before deployment.

## Last verified

- Pi `rapid.service`, `rapid-display.service`, and `rapid-log-status.service` are active.
  The native `rapid-pi` cutover remains pending final validation.
- The Pi has Drive, Timing, Vehicle, and Tyres touch pages.
- `\\rapid\Telemetry` is the working authenticated SMB location for finalized bundles.
- Native Windows companion source builds and passes its synthetic LD self-test.
- Five CTest targets passed on the Pi in Debug: network, archive, runtime/recorder,
  log status and protocol. Network checks covered UDP, HTTP, WebSocket history
  and disconnect finalization. Later upload integration and compatibility changes
  require another build/test pass.
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
- Native Pi/archive implementation is present; production cutover remains pending.
- The latest local sender-compatibility and validation fixes are not yet built on
  the Pi. The earlier 20-file deployed-source comparison predates this migration.
- Hosted CI results are available through GitHub Actions after publication.
- Four-simulator live acceptance and immediate lap publication remain open.
