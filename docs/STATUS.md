# Status — 2026-09-08

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
- Deployed native revision `7388eb6` passed all six Pi Release CTest targets:
  v4, network, archive, runtime/recorder, log status and protocol. Binary and dashboard
  hashes match the tested staging build; hosted Debug/Release tests also passed.
  Network checks cover UDP, HTTP, WebSocket history and disconnect finalization.
- A real journal event advanced the native HTTP status with warning severity.
- Boot before cleanup measured 25.904 seconds. Removed the obsolete seven-second
  rc.local sleep and failed framebuffer-copy launch; cold-boot improvement is unmeasured.
- Kiosk revision `9c4a5c9` publishes its X11 activation environment and avoids
  interactive keyring prompts. The post-v4 screenshot shows the dashboard.

## Current change

The AC1/Content Manager demo candidate adds fresh-physics gating, metadata refresh,
portable native launchers and config-relative local recording output. The
[demo guide](AC1_DEMO_GUIDE.md) contains prerequisites, rehearsal and recovery.
The steering-wheel PNG now has an explicit HTTP route and a binary-content
regression check. These changes are not yet verified on the production Pi:
the staging build was started before connectivity was lost; both the hostname
and last known IP were unreachable on September 8. Do not equate a prepared
package with a passed real AC driving/MoTeC rehearsal.
The Windows native build and isolated AC shared-memory/LD self-test passed on
September 8, covering process names, controls, metadata, lap timing, pause,
replay, packet counter restart and config-relative output. The tests use their
own mapping names and do not inject samples into a running simulator.

The Windows companion includes the tested/deployed quiet duplicate-launch fix
(`7b4db54`). The next deployment candidate prevents heartbeats from turning
retained values into graph samples, deduplicates dashboard polling by source
sample, and rejects malformed packets before recording changes. All six Pi
Release CTests and the isolated Chromium graph check passed. The exact staged
binary and validation log are recorded privately for the morning deployment.
The same dashboard deployment includes the optimized cartoon steering-wheel
asset derived from the supplied reference image.

Authenticated v4 is implemented in the Windows sender and Pi receiver. The final
isolated Pi Release build passed all six CTests, including actual Windows BCrypt
fixtures, validity/loss regression checks and replay persistence. A Windows-to-Pi
UDP check on separate ports verified two samples, one rejected replay and session
finalization. [Hosted Windows, Linux Debug and Linux Release checks for 7388eb6](https://github.com/2thgun/rapid/actions/runs/34157505987)
all passed, with Linux decoding fresh fixtures from the Windows job.
Production Pi and the installed Windows companion now use v4 with paired private
keys. An authenticated idle heartbeat on production port 9001 confirmed matching
keys without creating synthetic recordings. The companion starts with v4 at
Windows login; live simulator/MoTeC acceptance is still pending.
Previous binaries and configuration were preserved in `.old` backups.
See [v4 setup and protocol](TELEMETRY_V4.md) for pairing and rollback.

## Next deployment

Deploy the freshness candidate only while the recorder is idle. Preserve the
current `cpp/` tree as a new uniquely named `.old` backup, replace it with the
tested staged tree, and restart `rapid.service`; `rapid-display.service` must be
started again because it is part of `rapid.service`. Verify `/healthz`, `/api/live`
(`telemetry_fresh` and `telemetry_age_ms`), all three services, and the panel.
Keep the paired key, state database and telemetry directory. Use the companion
from the matching tested demo package for the AC freshness fix. A live driving
session remains the final acceptance.

Display output now goes only to the journal, preventing Xorg and Chromium output
from drawing over the dashboard. A three-second “Log updated” badge is driven by
the native C++ monitor. Full messages remain in the persistent journal, limited
to 100 MB and 14 days.

## Known limits

- Lap files are produced when a session bundle finalizes, not after each completed lap.
- Fallback sectors are distance thirds where canonical sector loops are unavailable.
- Native Pi/archive implementation is present. Live simulator and archive
  acceptance remain pending.
- Four-simulator live acceptance and immediate lap publication remain open.
