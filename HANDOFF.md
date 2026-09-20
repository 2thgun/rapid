# Current handoff

Read [PROJECT.md](PROJECT.md) for the working method and claim vocabulary.
History lives in `wiki/Development-Log.md`; this file only describes now.

## Checkpoint — 2026-09-20

Source `main` at `0d6d5cf` (PR #34, #35 and #36). Pull with
`git fetch && git pull --ff-only` (and the same for `wiki` on `master`) in an
older clone. Live coordination and claims are in `../developer/COWORK.md`; the
hardware runbook is `../developer/runbooks/hardware-session.md`.

Merged since the 2026-09-18 checkpoint: the `cpp/` tree split into per-component
folders, and the 0.9.9 release pipeline — a git-tag-derived package version,
hosted ARM64 CI with `-Werror`, and a `release.yml` that assembles a flashable
`.img.xz` from the ARM64 `.deb` and attaches the artifacts to a **draft** release
on a `v*` tag.

**Implemented, not hardware-verified:** first-boot TLS identity, setup AP and
owner enrollment; constrained hostname, Home Wi-Fi and rotation with rollback;
panel/setup touch calibration; the full pairing flow (TLS listener, approval,
envelope, Windows DPAPI client); companion download and manual `--pair`; the
browser SSH-key enrollment.

**Tested:** local gate at `0d6d5cf` passed Debug and Release 26/26 with the
Qt-display regression, launcher recovery and the Windows companion build and
self-test.

**Verified (hosted CI):** `main` `0d6d5cf` (run `35504332822`) — cpp-native
Debug/Release, cpp-native-arm64 Debug/Release with `-Werror`, Windows companion
and pairing fixtures. Release dry run `35504339636` produced the ARM64 `.deb`
and a flashable image without creating a release.

**Deployed:** the development Pi runs the older package from `7b5e910`. None of
the work above is installed there. No image has been flashed.

**Accepted:** nothing. Every open issue remains open against its full criteria;
#7 still needs real AC1/ACC sample evidence.

## Next

1. Hardware session (owner): clean ARM64 build, install the Pi package and the
   companion as a matched v4 pair, measured reboot, an AC1 drive with
   pause/alt-tab/garage/laps, rotation and touch, setup-page companion download
   and pairing, browser-key SSH login, and a MoTeC check.
2. Clean-device [release acceptance](https://github.com/2thgun/rapid/wiki/Release-Acceptance) ([#12](https://github.com/2thgun/rapid/issues/12))
   from a blank SD card and a clean Windows account. When it passes, tag
   `v0.9.9`; `release.yml` then creates a draft release to review and publish.
   Do not publish before acceptance.
3. Real ACE/iRacing acceptance ([#4](https://github.com/2thgun/rapid/issues/4)):
   only synthetic official-layout self-tests exist.

## Device and workspace

- Reach the Pi with `ssh rapid@rapid`. Check live services and confirm the
  recorder is idle first. Keep keys, configuration, the database and recordings
  in timestamped `.old` backups.
- Local Linux builds: `developer/WSL-BUILD.md`. The full local gate is
  `developer/Test-LocalCandidate.ps1` (pass `-SourceRoot` and `-BuildPrefix`).
- Session evidence goes in ignored `.local/sessions/YYYY-MM-DD-topic/`.
