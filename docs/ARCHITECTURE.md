# Architecture

```mermaid
flowchart LR
  Games[ACC / AC / ACE / iRacing] --> Companion[Windows C++ companion]
  Companion -->|UDP :9001| Pi[Pi C++ runtime]
  ACC[ACC broadcaster] -->|UDP :9000| Pi
  Pi --> Dashboard[GPIO touch dashboard]
  Journal[systemd journal] --> NativeLogs[C++ log monitor :8001]
  NativeLogs --> Dashboard
  Pi --> Recorder[MoTeC recorder]
  Recorder --> SMB[\\rapid\Telemetry]
  Recorder -. optional queue .-> Archive[Archive service]
```

The active Pi application exposes `/`, `/api/live`, `/telemetry`, `/api/v1/status`,
and `/api/v1/live`. `rapid-display.service` starts Chromium on the GPIO framebuffer.

The last verified production companion receiver supports JSON schemas v1–v3 on UDP
port 9001. Native v4 transport remains incomplete and must not be described as deployed.

`rapid-pi` combines TOML/environment
configuration, UDP reception, dashboard HTTP, WebSocket history, disk-backed LD
recording/recovery, SQLite persistence, firmware power monitoring, optional ACC
broadcast reception and durable archive uploads. `rapid-archive` implements
authenticated resumable ingest and verified publication. See the
[migration checkpoint](NATIVE_MIGRATION.md) for build and deployment evidence.

Dashboard assets live in `cpp/assets/`. HTTP clients use bounded worker/queue
counts; slow viewers do not block the producer's history buffer. Recorder state
is exposed through both live/status endpoints. Native ACC audit tables are named
`native_acc_packets` and `native_acc_laps`.

## Native log activity

`rapid-log-status` uses libsystemd directly. Its read-only endpoint is
`GET http://<pi>:8001/api/log-status`, returning `log_sequence` (integer),
`log_updated_at` (UTC timestamp or null), and `log_severity` (`info`, `warn`, `error`).
No journal messages or credentials are returned. Cross-origin reads let the
dashboard on port 8000 poll once per second. Keep both ports on the trusted LAN;
an HTTPS reverse proxy must also proxy the status endpoint.

The browser establishes a baseline on load, then displays “Log updated” for three
seconds when the sequence advances. A browser timer avoids clock-skew problems.
The native service coalesces bursts over five seconds and duplicate messages for
30 seconds, retaining at most 128 signatures. Higher-severity events can interrupt
the interval. Application/display output and service lifecycle events are watched.
