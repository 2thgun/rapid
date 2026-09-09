# Architecture

The Windows C++ companion reads simulator shared memory and forwards telemetry
to the Pi on UDP 9001. Authenticated binary v4 is the normal configuration.
The optional ACC broadcaster adapter uses UDP 9000.

- `rapid-pi` owns reception, live HTTP/WebSocket state, recording, recovery,
  SQLite persistence, power monitoring and optional archive uploads.
- `rapid-qt-display` consumes the local HTTP API and draws the five-page touch UI.
  Restarting the display does not restart the recorder.
- `rapid-log-status` monitors the journal and serves activity notices on port 8001.
- `rapid-network-mode` selects Home, AP or Off through NetworkManager.
- `rapid-archive` provides authenticated resumable recording uploads.

The runtime also serves a browser dashboard at `/` and engineering view at
`/telemetry`. Both use the same telemetry as Qt. Live state is available at
`/api/live` and `/api/v1/status`; the WebSocket history endpoint is
`/api/v1/live`.

Display assets are in `cpp/display/`; browser and recorder assets are in
`cpp/assets/`. HTTP workers and queues are bounded so slow viewers do not block
telemetry ingestion. Finalized recordings are exposed through the configured
read-only SMB share.

See [operations](OPERATIONS.md), [Qt display](QT_DISPLAY.md),
[protocol](TELEMETRY_V4.md) and [validation status](STATUS.md).
