# Current handoff

Read [PROJECT.md](PROJECT.md) for the working method and claim vocabulary.
History lives in `wiki/Development-Log.md`; this file only describes now.

## Checkpoint — 2026-09-18

Source `main` at `76c5ff0` (PR #34), carrying waves 1-3 and the 2026-09-18 task
wave. Pull with `git fetch && git pull --ff-only` (and the same for `wiki` on
`master`) in an older clone. Live coordination and claims are in
`../developer/COWORK.md`; the hardware runbook is
`../developer/runbooks/hardware-session.md`.

Merged since the last checkpoint: authenticated v4 only (the unauthenticated v3
JSON transport is removed on both ends), Qt-scene display rotation replacing
`xrandr`, companion download plus command-line-free `--pair`, browser-generated
setup SSH keys, private setup request files and a widened package verifier,
official-layout ACE/iRacing adapter self-tests, and a recorder durability fix
for the io crash guard.

**Implemented, not hardware-verified:** first-boot TLS identity, setup AP and
owner enrollment; constrained hostname, Home Wi-Fi and rotation with rollback;
panel/setup touch calibration; the full pairing flow (TLS listener, approval,
envelope, Windows DPAPI client); companion download and manual `--pair`; the
browser SSH-key enrollment.

**Tested:** local gate at `e9c47a6` passed Debug and Release 26/26 with the
Qt-display regression, launcher recovery and the Windows companion build and
self-test. The recorder fix was stress-run 8/8 + 8/8 under a loaded, CPU-pinned
condition. Not run: ARM64.

**Verified (hosted CI):** the `76c5ff0` merge commit and its PR head — cpp-native
Debug/Release, Windows companion and pairing fixtures all green. Earlier:
`36de9a8`.

**Deployed:** the development Pi runs the older package from `7b5e910`. None of
the work above is installed there. No image assembled or flashed.

**Accepted:** nothing. Every open issue remains open against its full criteria;
#7 still needs real AC1/ACC sample evidence.

## Next

1. Hardware session (owner): run `../developer/runbooks/hardware-session.md` —
   clean ARM64 build, install the Pi package and companion as a matched v4 pair,
   measured reboot, AC1 drive with pause/alt-tab/garage/laps, rotation and touch
   at 0°, setup-page download + `--pair`, browser-key SSH login, MoTeC check.
2. Software gaps that need no hardware: real ACE/iRacing acceptance (#4); the
   release pipeline in PR #35.

## Device and workspace

- Reach the Pi with `ssh rapid@rapid`. Check live services and confirm the
  recorder is idle first. Keep keys, configuration, the database and recordings
  in timestamped `.old` backups.
- Local Linux builds: `developer/WSL-BUILD.md`. The full local gate is
  `developer/Test-LocalCandidate.ps1` (pass `-SourceRoot` and `-BuildPrefix`).
- Session evidence goes in ignored `.local/sessions/YYYY-MM-DD-topic/`.
