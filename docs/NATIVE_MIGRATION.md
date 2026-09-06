# Native runtime migration — 2026-09-06

The native implementation provides the Pi application and archive ingest service.
Switch production only after native build and isolated checks pass.

## Implemented, awaiting cutover

- Extracted the existing dashboard and engineering page into `cpp/assets/`.
- Implemented TOML/environment configuration, HTTP/WebSocket serving, companion
  telemetry state, disk-backed LD recording, spool recovery, power monitoring,
  ACC broadcasting and durable archive uploads in C++.
- Existing device runtime remains available for rollback until the service switch.
- Native recorder retains successfully published spools with `.old` suffixes.
  Failed publication preserves the spool for retry/recovery.

## Build checkpoint

- Native Pi and archive targets compile on the Pi with distribution dependencies.
- Five Debug CTest targets passed: recorder/runtime, archive recovery, log
  status, protocol and real HTTP/UDP/WebSocket integration.
- Optimized builds and extended recorder-to-archive upload checks remain unverified.
- The maintained service unit now names the native executable; the device unit
  has not yet switched at this checkpoint. Check the acceptance criteria before
  treating native source as deployed.

## Remaining verification

Production cutover remains pending.
The latest code additionally handles the v3 companion's missing
session IDs/timestamps, resets sessions after waiting/disconnect, maps ABS, and
filters/validates telemetry fields before updating live state. Corresponding
regressions were added but have not yet been built or run.

Build and test the exact revision intended for deployment. Preserve a fresh
rollback backup before the service switch.

## Acceptance before production switch

1. Native build and tests pass on the Pi.
2. Isolated synthetic telemetry exercises dashboard APIs, WebSocket streaming,
   session finalization, LD channels, restart recovery and invalid input.
3. Save production service/source rollback copy before deployment.
4. Confirm recording is idle; switch service and inspect APIs, journals and panel.
5. Record the deployed revision and verification results.

Live simulator and MoTeC application acceptance are recorded separately from
synthetic checks. C++ implementation does not by itself establish those results.
