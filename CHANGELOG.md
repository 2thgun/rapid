# Changelog

Changes are unreleased until a version is tagged. Verification describes the
last completed checks, not a guarantee of current device state.

## Unreleased

- Added opt-in browser owner enrollment to the loopback setup service using a
  private activation token, bounded attempts and permanent owner-claim protection.
  Automatic AP bootstrap and physical token display are still pending.

- Fixed Wi-Fi AP/Off requests reporting success after NetworkManager errors;
  added regression checks for failure reporting and Home-to-AP recovery.

- Added the packaged Home/AP/Off network worker with Home startup and automatic
  AP recovery when the saved Home connection cannot be established.
- Added authenticated setup preference persistence and a first-boot state
  initializer. Image generation remains gated until the complete customer flow
  declares the `rapid-image-ready-v1` contract marker.
- Added native ACE and iRacing adapter fixtures and corrected their Windows test
  build compatibility.
- Clarified that v4 HMAC pairing requires private per-PC key material on the Pi;
  a non-secret verifier alone cannot authenticate symmetric packets.

- Corrected the browser and Qt wheel-speed labels to `rad/s`; telemetry and
  MoTeC recordings already used angular speed rather than vehicle speed.

- Moved guides and sanitized development logs to the GitHub wiki, pinned as the
  source repository's `wiki/` submodule. Added a clone-ready handoff and updated
  demo packaging to export offline guides from that documentation checkpoint.

- Restored the complete Qt dashboard data layout, fixed graph rendering and
  missing-channel/connection-loss gaps, and enlarged navigation and Wi-Fi controls.
- Added isolated Qt regression checks and enabled Qt builds in CI.
- Removed unused prototype C++ code, retired PowerShell telemetry runtimes,
  obsolete desktop autostart, and superseded migration/protocol notes.

- Added a portable paired AC1/Content Manager demo package, launchers and complete
  setup, rehearsal and troubleshooting instructions.
- AC telemetry now requires a new physics packet, rejects paused/replay data,
  and refreshes session metadata after loading. Isolated Windows shared-memory
  tests cover the actual adapter and original AC's car/track layout.
- Fixed the dashboard server's missing route for the cartoon steering-wheel PNG.

- Replaced the generic dashboard steering-wheel drawing with a lightweight,
  accurate cartoon asset based on the supplied three-spoke wheel reference.

- Graphs distinguish actual sample freshness from companion heartbeats, avoid
  plotting the same sample repeatedly, and resume polling after a stalled request.
- Invalid legacy sequence numbers are rejected before changing connection state
  or finalizing an existing recording.

- Duplicate companion launches now exit quietly instead of leaving an
  "already running" dialog and an extra process open.

### Authenticated v4 telemetry

- Connected the Windows binary encoder to the daemon and added native Pi decoding.
- Added shared-key configuration, HMAC verification, strict packet validation and
  persistent replay watermarks. A configured Pi key disables v3 fallback.
- Added real Windows encoder fixtures and cross-platform receiver/recording tests.
- Documented migration, wire layout and remaining loss/freshness limitations.

### Native migration — implementation checkpoint

- Implemented C++ Pi configuration, HTTP/WebSocket serving, UDP telemetry,
  disk-backed LD recording/recovery, power monitoring, ACC reception and uploads.
- Implemented C++ archive authentication, resumable uploads and crash-safe commit.
- Added native recorder, archive and network tests. The native production cutover
  and subsequent authenticated v4 deployment are complete; live simulator and
  MoTeC acceptance remain pending. See `https://github.com/2thgun/rapid/wiki/Validation-Status` for verification revisions.
- Added local compatibility handling for v3 senders without session IDs/timestamps,
  session reset on waiting/disconnect, ABS mapping and protected live-state fields.
  Automated native checks cover these compatibility paths.
- Removed deprecated legacy implementations from the maintained repository;
  preserved them in the private developer backup before deletion.

### Added

- Native C++ journal monitor with a metadata-only `/api/log-status` endpoint.
- Three-second dashboard log notification with severity indication.
- Native tests for filtering, severity escalation, duplicate expiry, bounded
  history and exclusion of raw log messages from the status response.
- GitHub CI workflows, issue forms, pull request template, contributor guidance,
  architecture, operations and prioritized delivery backlog.

### Changed

- Separated maintained repository content from private developer artifacts;
  preserved the original workspace structure in `.old` backups.
- Routed display output to persistent, bounded journals to keep console messages
  off the dashboard.
- Removed dashboard service dependencies on network-online readiness.
- Made Windows companion builds support explicit MSVC or Zig selection and
  separate output/cache directories.

### Verification — 2026-09-06

- Latest saved device checks: all three services active, native status reachable
  from Windows, and a warning notification visible in a captured dashboard image.
- All 20 compared deployed source files match the local repository.
- Pi Debug CTest: 2/2; Windows companion build and synthetic LD self-test passed.
- Expanded native log tests passed locally with optimization and `NDEBUG`.
  These additional tests have not been copied to the Pi; runtime code is unchanged.
- Hosted CI and live four-simulator acceptance remain pending. Overall boot still
  takes about 25 seconds. See [status](https://github.com/2thgun/rapid/wiki/Validation-Status) for remaining limitations.
