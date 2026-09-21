# Current handoff

Read [PROJECT.md](PROJECT.md) for the working method and claim vocabulary.
History lives in `wiki/Development-Log.md`; this file only describes now.

## Checkpoint — 2026-09-21

Source `main` at `dbbf562` (PR #39 merged from `810132f`). Pull with
`git fetch && git pull --ff-only` (and the same for `wiki` on `master`) in an
older clone. Live coordination and claims are in `../developer/COWORK.md`; the
hardware runbook is `../developer/runbooks/hardware-session.md`.

**Implemented, not hardware-verified:** the full pairing flow (TLS listener,
approval, envelope, Windows DPAPI client); companion download and manual `--pair`;
the fresh-device first-boot AP/enrollment flow end to end (only its pieces have
been exercised).

**Verified (hosted CI):** `main` `dbbf562` (run `35517845254`) — cpp-native
Debug/Release, cpp-native-arm64 Debug/Release with `-Werror`, Windows companion
and pairing fixtures.

**Deployed and verified:** the development Pi runs `0.9.9~dev+dbbf562`. PR #39's
`/run/rapid-apply` `226/NAMESPACE` fix is confirmed across a live upgrade, a soft
reboot and a cold boot; during the setup-AP segment `rapid-setup` bound and served
the setup page. A **follow-up defect** was then reproduced: the four queue
consumers share `RuntimeDirectory=rapid-apply`, so a consumer stopping removes the
queue while `rapid-setup` runs (`developer/tasks/shared-runtime-directory-queue.md`).

**Also found in the setup-AP segment:** the AP came up secured rather than open
(#22), the activation token was not shown on the panel, the Qt-scene rotation
does not rotate the X cursor, and setup-page UX/credential changes were requested.
Briefs are in `developer/tasks/` and the dispatch table.

ARM64 packages can now be built off the driving Pi on an ARM64 host; see
[ARM64 build host](https://github.com/2thgun/rapid/wiki/ARM64-Build-Host).

**Accepted:** nothing. Every open issue remains open against its full criteria;
#7 still needs real AC1/ACC sample evidence.

## Next

1. Fix the shared-queue lifetime defect, then re-verify sequential setup requests
   and browser SSH-key enrollment on the Pi. Remove the temporary dev-Pi
   overrides and the temporary SSH key at session close.
2. Resolve the MoTeC i2 issue (#19/#29): the ACC-generated `.ld` works, the
   raPId sample does not.
3. Clean-device [release acceptance](https://github.com/2thgun/rapid/wiki/Release-Acceptance) ([#12](https://github.com/2thgun/rapid/issues/12))
   from a blank SD card and a clean Windows account. When it passes, tag
   `v0.9.9`; `release.yml` then creates a draft release to review and publish.
   Do not publish before acceptance.
4. Real ACE/iRacing acceptance ([#4](https://github.com/2thgun/rapid/issues/4)):
   only synthetic official-layout self-tests exist.

## Device and workspace

- Reach the Pi with `ssh rapid@rapid`. Check live services and confirm the
  recorder is idle first. Keep keys, configuration, the database and recordings
  in timestamped `.old` backups.
- Local Linux builds: `developer/WSL-BUILD.md`. The full local gate is
  `developer/Test-LocalCandidate.ps1` (pass `-SourceRoot` and `-BuildPrefix`).
- Session evidence goes in ignored `.local/sessions/YYYY-MM-DD-topic/`.
