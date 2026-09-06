# Native runtime migration — 2026-09-07

The native implementation provides the Pi application and archive ingest service.
The Pi dashboard and recorder cutover completed after native build and isolated checks.

## Implemented and deployed on the Pi

- Extracted the existing dashboard and engineering page into `cpp/assets/`.
- Implemented TOML/environment configuration, HTTP/WebSocket serving, companion
  telemetry state, disk-backed LD recording, spool recovery, power monitoring,
  ACC broadcasting and durable archive uploads in C++.
- The preserved legacy device runtime remains available for rollback.
- Native recorder retains successfully published spools with `.old` suffixes.
  Failed publication preserves the spool for retry/recovery.

## Build checkpoint

- Native Pi and archive targets compile on the Pi with distribution dependencies.
- Five Debug CTest targets passed: recorder/runtime, archive recovery, log
  status, protocol and real HTTP/UDP/WebSocket integration.
- The optimized build passed its four release-safe test targets. The protocol
  target is assertion-dependent and is covered by the Debug test run.
- `rapid.service` now runs `cpp/build/rapid-pi`. The kiosk and native log monitor
  are active, and local dashboard, live-state and log-status endpoints responded
  after the service switch with zero restarts.

## Remaining verification

Live simulator, recording, archive and MoTeC application acceptance remain open.
The deployed source handles the v3 companion's missing session IDs/timestamps,
resets sessions after waiting/disconnect, maps ABS, and filters/validates telemetry
fields before updating live state.

## Cutover evidence

1. Native Debug build passed five CTest targets on the Pi.
2. Native Release build passed four release-safe CTest targets on the Pi.
3. A full pre-cutover runtime, configuration and unit snapshot is preserved under
   `/home/rapid/raPId/.backup/native-cutover-85ff713-20260907.old/`.
4. The service runs `rapid-pi`; the dashboard exposes the Graphs asset and
   `/api/live`, while the native log monitor answers on port 8001.

Live simulator and MoTeC application acceptance are recorded separately from
synthetic checks. C++ implementation does not by itself establish those results.
