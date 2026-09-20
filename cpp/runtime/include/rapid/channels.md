# Canonical MoTeC channel layout (#29)

This is the single documented source for the channel table written into every
raPId `.ld` recording. It is implemented by `channels.inc` (the Pi recorder)
and mirrored row-for-row by the `kChannels` table in
`companion/native/rapid-telemetry-daemon.cpp` (the companion is a separate
Visual Studio project and cannot `#include` the `.inc`). The independent LD
read-back test (`cpp/runtime/tests/native_recorder_tests.cpp`) pins this table
so a drift fails the gate.

## Decision

raPId uses **one sim-agnostic layout for every simulator**. The reference is
the channel set of ACC's own MoTeC export, which is a MoTeC **ADL** device file
and therefore uses MoTeC's standard ADL channel identifiers
(`SPEED`, `RPMS`, `THROTTLE`, `BRAKE`, `GEAR`, `G_LAT`, `G_LON`,
`SUS_TRAVEL_*`, `TYRE_PRESS_*`, `WHEEL_SPEED_*`, `STEERANGLE`, `TIME`, ...).
These identifiers are MoTeC's own vocabulary, not ACC-invented words, so
adopting them is the "unified layout modelled primarily on ACC's MoTeC export"
required by issue #29, not renaming raPId into ACC-specific terminology.

Two hard rules constrain how far that adoption goes:

1. **One unit per channel.** A channel never carries two different physical
   units, and its declared unit always describes the value actually stored.
2. **No value-semantics change here.** The stored value is unchanged. A rename
   is only adopted when raPId already records the same physical quantity in the
   same dimension as the reference, so i2 can convert between the declared unit
   and the unit a worksheet displays.

Where the reference identifier would require changing the stored value (or
where the reference channel is a different quantity), the identifier is **not**
adopted and the canonical channel is kept; the gap is documented and tracked
for a separate change. See "Not adopted" below.

## Per-sim behaviour

All simulators write exactly these names and units through the same C++
`Recorder`; the only per-sim difference is that a simulator which does not
provide a quantity leaves that channel's samples at zero (recorded in the
manifest's `channel_available_samples`). No channel is renamed or re-united per
sim.

The `key` column is the companion `Frame` field that feeds the channel. The
sim-specific shared-memory field that fills each `Frame` field lives in the
adapters in `companion/native/rapid-telemetry-daemon.cpp`; the ACC column below
names the source in ACC's own physics page / export.

## Table

| key | name | short | unit | dec | ACC source |
| --- | --- | --- | --- | --- | --- |
| elapsed | Time | Time | s | 3 | session time (ACC `TIME`) |
| throttle | THROTTLE | Throttle | % | 1 | `gas` |
| brake | BRAKE | Brake | % | 1 | `brake` |
| fuel | Fuel Level | Fuel | l | 2 | (no ACC export channel) |
| gear | GEAR | Gear | | 0 | `gear` |
| rpm | RPMS | RPM | 1/min | 0 | `rpms` |
| steering_angle | Steered Angle | Steer | | 3 | `steerAngle` (normalised) |
| speed_kmh | SPEED | Speed | km/h | 1 | `speedKmh` |
| velocity_x | Velocity Lat | Vel Lat | m/s | 2 | (ACC export has no velocity) |
| velocity_y | Velocity Vert | Vel Vert | m/s | 2 | - |
| velocity_z | Velocity Long | Vel Long | m/s | 2 | - |
| g_x | G_LAT | G Lat | g | 2 | `accG[0]` |
| g_y | G Force Vert | G Vert | g | 2 | (no ACC vertical G) |
| g_z | G_LON | G Long | g | 2 | `accG[2]` |
| wheel_slip_fl | Wheel Slip FL | Slip FL | | 2 | `wheelSlip[0]` |
| wheel_slip_fr | Wheel Slip FR | Slip FR | | 2 | `wheelSlip[1]` |
| wheel_slip_rl | Wheel Slip RL | Slip RL | | 2 | `wheelSlip[2]` |
| wheel_slip_rr | Wheel Slip RR | Slip RR | | 2 | `wheelSlip[3]` |
| pressure_fl | TYRE_PRESS_LF | Press FL | psi | 1 | `wheelsPressure[0]` |
| pressure_fr | TYRE_PRESS_FR | Press FR | psi | 1 | `wheelsPressure[1]` |
| pressure_rl | TYRE_PRESS_RL | Press RL | psi | 1 | `wheelsPressure[2]` |
| pressure_rr | TYRE_PRESS_RR | Press RR | psi | 1 | `wheelsPressure[3]` |
| wheel_speed_fl | Wheel Speed FL | WhlSp FL | rad/s | 1 | `wheelAngularSpeed[0]` |
| wheel_speed_fr | Wheel Speed FR | WhlSp FR | rad/s | 1 | `wheelAngularSpeed[1]` |
| wheel_speed_rl | Wheel Speed RL | WhlSp RL | rad/s | 1 | `wheelAngularSpeed[2]` |
| wheel_speed_rr | Wheel Speed RR | WhlSp RR | rad/s | 1 | `wheelAngularSpeed[3]` |
| core_temp_fl | Tyre Temp FL | Temp FL | C | 1 | `tyreCoreTemperature[0]` |
| core_temp_fr | Tyre Temp FR | Temp FR | C | 1 | `tyreCoreTemperature[1]` |
| core_temp_rl | Tyre Temp RL | Temp RL | C | 1 | `tyreCoreTemperature[2]` |
| core_temp_rr | Tyre Temp RR | Temp RR | C | 1 | `tyreCoreTemperature[3]` |
| suspension_fl | SUS_TRAVEL_LF | Susp FL | m | 3 | `suspensionTravel[0]` |
| suspension_fr | SUS_TRAVEL_FR | Susp FR | m | 3 | `suspensionTravel[1]` |
| suspension_rl | SUS_TRAVEL_RL | Susp RL | m | 3 | `suspensionTravel[2]` |
| suspension_rr | SUS_TRAVEL_RR | Susp RR | m | 3 | `suspensionTravel[3]` |
| tc | TC | TC | | 0 | `tc` |
| heading | Heading | Heading | rad | 3 | `heading` |
| pitch | Pitch | Pitch | rad | 3 | `pitch` |
| roll | Roll | Roll | rad | 3 | `roll` |
| damage_front | Damage Front | Dmg F | | 2 | `carDamage[0]` |
| damage_rear | Damage Rear | Dmg R | | 2 | `carDamage[1]` |
| damage_left | Damage Left | Dmg L | | 2 | `carDamage[2]` |
| damage_right | Damage Right | Dmg Rgt | | 2 | `carDamage[3]` |
| damage_center | Damage Center | Dmg C | | 2 | `carDamage[4]` |
| pit_limiter | Pit Limiter | Pit Lim | | 0 | `pitLimiterOn` |
| abs | ABS | ABS | | 0 | `abs` |
| lap_number | Lap Number | Lap | | 0 | (lap state, not a channel) |
| current_lap_ms | Lap Time | Lap Time | s | 3 | (lap state) |
| lap_position | Lap Position | Lap Pos | % | 1 | (lap state) |

## Adopted from the ACC/MoTeC ADL reference

`THROTTLE`, `BRAKE`, `GEAR`, `RPMS`, `SPEED`, `G_LAT`, `G_LON`,
`SUS_TRAVEL_LF/RF/LR/RR`, `TYRE_PRESS_LF/RF/LR/RR`, `TC`, `ABS`.

The ACC workspace binds traces by these exact Ids (and i2 maps the same Ids to
its built-in MoTeC channels such as `Engine RPM`, `Throttle Pos`,
`G Force Lat/Long`, `Corr Speed`). The unit shown is the truthful unit of the
stored value; where it differs from the reference's unit (ACC exports `SPEED`
in m/s, `SUS_TRAVEL_*` in mm, `G_LAT/G_LON` in m/s2) the difference is a pure
unit scaling of the same dimension, which i2 performs from the channel's
declared unit.

## Not adopted (deliberate), with reason

| raPId channel | reference identifier | why not |
| --- | --- | --- |
| Steered Angle | `STEERANGLE` (deg) | raPId stores normalised -1..1, not degrees. Adopting the reference name would mislabel the value. |
| Wheel Speed FL..RR | `WHEEL_SPEED_*` (m/s) | raPId stores angular speed in rad/s; rad/s is not a unit scaling of m/s. |
| Tyre Temp FL..RR | `TYRE_TAIR_*` (C) | raPId stores tyre **core** temperature; `TYRE_TAIR` is tyre **air** temperature, a different quantity. |
| Heading | `ROTY` (rad/s) | raPId stores a heading angle; `ROTY` is a yaw rate. |
| - | `CLUTCH`, `BRAKE_TEMP_*`, `ROTY`, `LAP_BEACON`, `EN_*`, `BUMPSTOP*` | raPId does not record these quantities. |

These are inherent workspace gaps, not naming choices. Closing them requires
changing what the adapters/recorder store (normalised steering -> degrees,
angular -> linear wheel speed, adding clutch/brake-temp/tyre-air/yaw-rate), not
channel metadata, and is tracked as a separate proposed task. A richer option
for 1.0.0 is to ship a raPId i2 workspace (#30) whose aliases map raPId names
onto the ACC workspace's Ids.

## Known differences from the reference file

- **Sample rate.** The reference uses per-channel rates (20/50/60/100/200 Hz);
  raPId records every channel at one session rate (the `sample_rate_hz` on the
  manifest, 50 Hz by default). High-rate damper histograms are therefore
  coarser than ACC's 200 Hz export.
- **Decimals.** The reference writes `dec = 0` for every channel and lets each
  worksheet override the display; raPId writes a per-channel `dec` for a
  sensible default. This is display-only and does not affect binding.
- **Short names.** The reference leaves short names empty; raPId keeps its own.
- **Channel order.** raPId keeps its historical order (Time first, lap state
  last). i2 binds by name, not position, so this does not affect worksheets.
