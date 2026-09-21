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
| elapsed | Time | Time | s | 0 | session time (ACC `TIME`) |
| throttle | THROTTLE | Throttle | % | 0 | `gas` |
| brake | BRAKE | Brake | % | 0 | `brake` |
| fuel | Fuel Level | Fuel | l | 0 | (no ACC export channel) |
| gear | GEAR | Gear | | 0 | `gear` |
| rpm | RPMS | RPM | 1/min | 0 | `rpms` |
| steering_angle | Steered Angle | Steer | | 0 | `steerAngle` (normalised) |
| speed_kmh | SPEED | Speed | km/h | 0 | `speedKmh` |
| velocity_x | Velocity Lat | Vel Lat | m/s | 0 | (ACC export has no velocity) |
| velocity_y | Velocity Vert | Vel Vert | m/s | 0 | - |
| velocity_z | Velocity Long | Vel Long | m/s | 0 | - |
| g_x | G_LAT | G Lat | g | 0 | `accG[0]` |
| g_y | G Force Vert | G Vert | g | 0 | (no ACC vertical G) |
| g_z | G_LON | G Long | g | 0 | `accG[2]` |
| wheel_slip_fl | Wheel Slip FL | Slip FL | | 0 | `wheelSlip[0]` |
| wheel_slip_fr | Wheel Slip FR | Slip FR | | 0 | `wheelSlip[1]` |
| wheel_slip_rl | Wheel Slip RL | Slip RL | | 0 | `wheelSlip[2]` |
| wheel_slip_rr | Wheel Slip RR | Slip RR | | 0 | `wheelSlip[3]` |
| pressure_fl | TYRE_PRESS_LF | Press FL | psi | 0 | `wheelsPressure[0]` |
| pressure_fr | TYRE_PRESS_RF | Press FR | psi | 0 | `wheelsPressure[1]` |
| pressure_rl | TYRE_PRESS_LR | Press RL | psi | 0 | `wheelsPressure[2]` |
| pressure_rr | TYRE_PRESS_RR | Press RR | psi | 0 | `wheelsPressure[3]` |
| wheel_speed_fl | Wheel Speed FL | WhlSp FL | rad/s | 0 | `wheelAngularSpeed[0]` |
| wheel_speed_fr | Wheel Speed FR | WhlSp FR | rad/s | 0 | `wheelAngularSpeed[1]` |
| wheel_speed_rl | Wheel Speed RL | WhlSp RL | rad/s | 0 | `wheelAngularSpeed[2]` |
| wheel_speed_rr | Wheel Speed RR | WhlSp RR | rad/s | 0 | `wheelAngularSpeed[3]` |
| core_temp_fl | Tyre Temp FL | Temp FL | C | 0 | `tyreCoreTemperature[0]` |
| core_temp_fr | Tyre Temp FR | Temp FR | C | 0 | `tyreCoreTemperature[1]` |
| core_temp_rl | Tyre Temp RL | Temp RL | C | 0 | `tyreCoreTemperature[2]` |
| core_temp_rr | Tyre Temp RR | Temp RR | C | 0 | `tyreCoreTemperature[3]` |
| suspension_fl | SUS_TRAVEL_LF | Susp FL | m | 0 | `suspensionTravel[0]` |
| suspension_fr | SUS_TRAVEL_RF | Susp FR | m | 0 | `suspensionTravel[1]` |
| suspension_rl | SUS_TRAVEL_LR | Susp RL | m | 0 | `suspensionTravel[2]` |
| suspension_rr | SUS_TRAVEL_RR | Susp RR | m | 0 | `suspensionTravel[3]` |
| tc | TC | TC | | 0 | `tc` |
| heading | Heading | Heading | rad | 0 | `heading` |
| pitch | Pitch | Pitch | rad | 0 | `pitch` |
| roll | Roll | Roll | rad | 0 | `roll` |
| damage_front | Damage Front | Dmg F | | 0 | `carDamage[0]` |
| damage_rear | Damage Rear | Dmg R | | 0 | `carDamage[1]` |
| damage_left | Damage Left | Dmg L | | 0 | `carDamage[2]` |
| damage_right | Damage Right | Dmg Rgt | | 0 | `carDamage[3]` |
| damage_center | Damage Center | Dmg C | | 0 | `carDamage[4]` |
| pit_limiter | Pit Limiter | Pit Lim | | 0 | `pitLimiterOn` |
| abs | ABS | ABS | | 0 | `abs` |
| lap_number | Lap Number | Lap | | 0 | (lap state, not a channel) |
| current_lap_ms | Lap Time | Lap Time | s | 0 | (lap state) |
| lap_position | Lap Position | Lap Pos | % | 0 | (lap state) |

## Adopted from the ACC/MoTeC ADL reference

`THROTTLE`, `BRAKE`, `GEAR`, `RPMS`, `SPEED`, `G_LAT`, `G_LON`,
`SUS_TRAVEL_LF/RF/LR/RR`, `TYRE_PRESS_LF/RF/LR/RR`, `TC`, `ABS`.

The corner order is MoTeC's own: **`LF`/`RF`/`LR`/`RR`** (left-front,
right-front, left-rear, right-rear), exactly as ACC's export and the
`base_ACC` workbook write it (`SUS_TRAVEL_RF`, `SUS_TRAVEL_LR`,
`TYRE_PRESS_RF`, `TYRE_PRESS_LR`). The first #29 implementation used the
`FL/FR/RL/RR` order instead for four rows, so the right-front and left-rear
suspension/tyre channels carried names the ACC workspace does not reference.
Corrected here; the `key`/`scale` columns were untouched, so the stored data
and its corner meaning are unchanged.

The ACC workspace binds traces by these exact Ids (and i2 maps the same Ids to
its built-in MoTeC channels such as `Engine RPM`, `Throttle Pos`,
`G Force Lat/Long`, `Corr Speed`). The unit shown is the truthful unit of the
stored value; where it differs from the reference's unit (ACC exports `SPEED`
in m/s, `SUS_TRAVEL_*` in mm, `G_LAT/G_LON` in m/s2) the difference is a pure
unit scaling of the same dimension, which i2 performs from the channel's
declared unit.

## Which i2 workspace these names bind

i2 resolves a workbook trace against a logged channel by its Id = the LD
channel **name** (exact, case-insensitive). That is why the layout is tied to
the workspace, and why the same names cannot serve every workspace:

- The **generic MoTeC profile** workbooks (`i2\Profiles\Circuit\Template`,
  the default when no project workspace is open) reference MoTeC's canonical
  names: `Engine RPM`, `Throttle Pos`, `G Force Lat`, `G Force Long`,
  `Steered Angle`, `Gear`, `Corr Speed`. Before #29 raPId emitted those names,
  which is why the generic worksheet bound `Engine RPM` (and only it, since
  the generic Engine/Driver workbooks put the other raPId values on different
  traces). #29 replaced them with the ACC/MoTeC ADL identifiers, so the
  **generic profile is now expected to bind nothing** — that is a consequence
  of choosing the ACC-derived layout, not a raPId defect.
- The ACC-derived **`base_ACC`** workbook (`Base.i2wkb`) references `SPEED`,
  `RPMS`, `GEAR`, `BRAKE`, `THROTTLE`, `STEERANGLE`, `WHEEL_SPEED_*`,
  `SUS_TRAVEL_*` plus the derived `glat`/`glong`, `Oversteer` and
  `Damper Vel *`. raPId now writes those names for the channels whose stored
  quantity matches; `STEERANGLE` and `WHEEL_SPEED_*` stay empty by design
  (see "Not adopted").

The way to make the generic profile useful is **not** to rename channels back,
because no single sim-agnostic name satisfies both workspaces. It is to ship a
raPId workspace (#30) whose aliases map the canonical names onto the MoTeC
profile's Ids. Tracked separately.

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
- **Decimals.** Both files write `dec = 0` on every channel. raPId previously
  wrote a per-channel `dec` as a display default; that was removed because the
  LD channel header's scaling fields are not safe to use as display precision.
  The community `ldparser` (the parser this format was reverse-engineered
  with) computes `value = (raw / scale * 10^-dec + shift) * mul` for **every**
  dtype including float32, so a nonzero `dec` risks i2 scaling a stored float
  by `10^-dec` (e.g. `THROTTLE` 80 -> 8, `SUS_TRAVEL` 0.03 -> 0.00003).
  Display precision belongs to the workspace override, as it does for ACC's own
  export. Pinned by `native_recorder_tests.cpp` (`float dec must be 0`). A
  separate reverse-engineered spec (racingmagick) claims float channels need no
  conversion, so the exact i2 behaviour is still an owner-i2 check; keeping
  `dec = 0` makes raPId identical to the known-good reference either way.
- **Short names.** The reference leaves short names empty; raPId keeps its own.
- **Channel order.** raPId keeps its historical order (Time first, lap state
  last). i2 binds by name, not position, so this does not affect worksheets.
