# Changelog

Changes are unreleased until a version is tagged. Verification describes the
last completed checks, not a guarantee of current device state.

## Unreleased

### Native migration — implementation checkpoint

- Implemented C++ Pi configuration, HTTP/WebSocket serving, UDP telemetry,
  disk-backed LD recording/recovery, power monitoring, ACC reception and uploads.
- Implemented C++ archive authentication, resumable uploads and crash-safe commit.
- Added native recorder, archive and network tests; the initial five Debug suites
  passed on the Pi. Optimized/extended test results and deployment remain pending.
- Added local compatibility handling for v3 senders without session IDs/timestamps,
  session reset on waiting/disconnect, ABS mapping and protected live-state fields.
  These later changes still require a build/test pass.
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
  takes about 25 seconds. See [status](docs/STATUS.md) for remaining limitations.
