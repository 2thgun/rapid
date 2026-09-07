# Authenticated telemetry v4

The native Windows companion sends binary v4 packets to the native Pi receiver
over UDP port 9001. Both ends use the same 256-bit key. v4 authenticates packet
contents with HMAC-SHA256; it does not encrypt telemetry or authenticate the Pi
back to the sender. Keep dashboard HTTP and SMB access on a trusted network.

## Configuration and migration

Build both components from the same revision and run the checks in
[Testing](TESTING.md). Older installed binaries may only support v3.

1. Generate 32 random bytes as 64 hexadecimal characters, for example using
   `openssl rand -hex 32` on the Pi. Store the key privately; never commit it or
   use the public fixture key for a deployment.
2. Set `companion_key = "<64 hex characters>"` under `[app]` in the Pi TOML
   configuration, or set `RAPID_COMPANION_KEY` in its service environment.
   Preserve the previous configuration as a `.old` backup first. Invalid keys
   fail startup; a configured key disables unauthenticated v1-v3 packets.
3. Copy the same hex string into a private text file on the Windows PC, then run:

   ```powershell
   .\rapid-telemetry-daemon.exe --protocol v4 --pi-host rapid --auth-key-file .\telemetry.key
   ```

4. Restart the Pi runtime while recording is idle. Start driving and check
   `/api/live`: `schema_version` must be `4`, `samples_received` must advance,
   and `recording` must be true. An HTTP health response alone cannot validate UDP.

For a persistent companion installation, set `protocol=v4` and
`auth_key_file=C:\absolute\private\telemetry.key` in
`%LOCALAPPDATA%\raPId\daemon.conf`. The companion also accepts
`RAPID_TELEMETRY_KEY`; the Pi environment variable has a different name.
The existing ACC broadcaster `protocol_version` setting is unrelated to v4.

To keep an older companion working, leave the Pi key empty and launch explicitly
with `--protocol v3`. There is no automatic fallback from v4 to v3. Rollback
requires restoring the previous binary/configuration together and restarting
while idle. Changing a key requires updating both ends.

## Wire contract

All integers and IEEE-754 float32 values are little-endian. Each datagram is a
52-byte header, the declared payload, then a 32-byte HMAC over header and payload.
The receiver caps datagrams at 4096 bytes and verifies the MAC before parsing or
changing session state.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `RPD4` |
| 4 | 1 | Version: 4 |
| 5 | 1 | Type: 1 telemetry, 2 metadata, 3 status |
| 6 | 1 | Simulator: 1 ACC, 2 AC, 3 ACE, 4 iRacing |
| 7 | 1 | Flags: bit 0 active run, bit 1 ended |
| 8 | 2 | Header length: 52 |
| 10 | 2 | Payload length, excluding HMAC |
| 12 | 2 | Channel schema: 1 |
| 14 | 2 | Channel count: 48 |
| 16 | 2 | Sample rate: 1-100 Hz |
| 18 | 2 | Reserved: zero |
| 20 | 16 | Random, nonzero run/control-stream identifier |
| 36 | 8 | Sequence, shared by all packet types in the stream |
| 44 | 8 | Microseconds since stream start, monotonic |

Sequence and timestamp values are limited to `2^53-1` for exact API representation.
Flags must agree with packet type and status. Simulator and active/control identity
cannot change within a stream; timestamps cannot move backward.

Telemetry payload is 208 bytes: validity mask (uint64), completed lap time
(int32 ms), delta (int32 ms), then 48 float32 channels. The entire datagram is
292 bytes. Mask bits follow this zero-based order:

```text
elapsed, throttle, brake, fuel, gear, rpm, steering_angle, speed_kmh,
velocity_x, velocity_y, velocity_z, g_x, g_y, g_z,
wheel_slip_fl, wheel_slip_fr, wheel_slip_rl, wheel_slip_rr,
pressure_fl, pressure_fr, pressure_rl, pressure_rr,
wheel_speed_fl, wheel_speed_fr, wheel_speed_rl, wheel_speed_rr,
core_temp_fl, core_temp_fr, core_temp_rl, core_temp_rr,
suspension_fl, suspension_fr, suspension_rl, suspension_rr,
tc, heading, pitch, roll, damage_front, damage_rear, damage_left,
damage_right, damage_center, pit_limiter, abs_activity, lap_number,
current_lap_ms, lap_position
```

Unknown mask bits, missing RPM/steering/G-force channels, nonfinite valid values,
and out-of-range controls are rejected. Missing optional channels become null in
live state. Gear, lap number and current lap time must have integral values.

Metadata payload contains track, car, driver and session strings in that order.
Each has a uint16 byte length followed by at most 512 UTF-8 bytes, without a NUL.
Metadata is sent before active data and repeated once per second so a receiver
that starts mid-session or loses the initial packet can join.

Status payload contains state (uint8: 0 waiting, 1 ready, 2 driving, 3 ended),
reserved zero (uint8), text byte length (uint16), sent packet count (uint64), then
up to 512 UTF-8 text bytes. Text and sent count are informational. Waiting/ready
use a separate random control-stream ID; ended retires the active run.

## Replay, loss and recording

SQLite `v4_runs` stores accepted sequence/timestamp watermarks and retired IDs.
Duplicate, out-of-order and retired-stream packets are rejected across receiver
restarts. A new accepted stream retires earlier streams. Preserve the state
database when restarting/upgrading; deleting it also deletes replay history.
Watermarks use the database's FULL synchronous WAL transactions before packets
are accepted. Retired IDs are retained; automatic pruning is not implemented.

This is not a challenge-response protocol: an authenticated stream never seen by
this database cannot be proven recent. Anyone possessing the shared key can
create a new accepted stream. Rotate the key if it becomes exposed.

The dashboard exposes `packets_auth_failed`, `packets_invalid`,
`packets_replayed` and `packets_lost`. Loss counts gaps in the shared wire sequence,
which can include metadata and status. Such gaps do not synthesize recording
samples. LD files contain received samples at the configured rate; they do not
reconstruct exact elapsed time across UDP loss. Live events retain sender times.
An ended/ready/waiting packet or the existing disconnect timeout finalizes the
session. Live simulator and MoTeC acceptance still require physical testing.
