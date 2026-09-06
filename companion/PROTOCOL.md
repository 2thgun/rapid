# raPId companion UDP protocol

The Windows daemon sends one UTF-8 JSON datagram per sample to the configured Pi
host and port. Version 3 carries the complete normalized shared-memory frame so
the Pi can record it. Receivers must ignore unknown
versions and unexpected source hosts.

Current source targets hostname `rapid` at 50 Hz by default; older binaries and
the legacy wrapper default to `255.255.255.255` at 10 Hz. The Pi learns and locks
to the first valid private sender while its packets remain live. The source's
new v4 selector/key validation is not connected to its active v3 send path; use
`--protocol v3` for explicit compatibility until that integration is completed.

Example version-3 telemetry at 10 Hz (rate is configurable):

```json
{
  "version": 3,
  "type": "telemetry",
  "simulator": "ACC",
  "sample_rate_hz": 10,
  "track_name": "Spa",
  "telemetry": {
    "rpm": 6123,
    "throttle": 0.8,
    "brake": 0.0,
    "steering_angle": -0.25,
    "g_x": 0.1,
    "g_y": 1.02,
    "g_z": -0.05
  }
}
```

After a supported simulator EXE starts, the daemon sends a status heartbeat once
per second while it waits for or reads that simulator:

```json
{"version":3,"type":"status","state":"waiting","simulator":null,"sample_rate_hz":10}
```

`state` is `waiting`, `ready`, or `driving`. There is intentionally no heartbeat
or network socket while no supported simulator EXE is running, so the Pi shows the
daemon as absent during that fully dormant state.

`simulator` is one of `ACC`, `AC`, `ACE`, or `iRacing`. RPM is an integer from
0–20,000. Steering is radians. G-force values are in g and must be finite.

The nested `telemetry` object contains all 48 normalized MoTeC channels when
available, including:

- `gear`, using `-1` reverse, `0` neutral, and positive forward gears
- `speed_kmh`
- `current_lap_ms`, `completed_lap_ms`, and `delta_ms`
- `lap_number`
- throttle, brake, fuel, velocity, G force, wheel slip/speed, tyre pressure and
  temperature, suspension travel, assists, orientation, damage, and lap position

Session metadata (`track_name`, `car_model`, `driver_name`, and `session_name`)
is carried beside the telemetry object.

Versions 1 and 2 remain accepted for compatibility. Only version 3 is complete
enough for Pi-side `.ld` recording.

Packets are intentionally small enough for a single UDP datagram. They currently
have source-host filtering but no authentication; keep port 9001 on a trusted LAN.
