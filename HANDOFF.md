# Current handoff

Read [PROJECT.md](PROJECT.md) for the working method and claim vocabulary.
History lives in `wiki/Development-Log.md`; this file only describes now.

## Checkpoint — 2026-09-15

Source: touchscreen calibration (#9) and owner-confirmed orientation changes
(#11), as local commits on top of `358f26f` that have not been pushed.

**Implemented, not hardware-verified:**

- First boot: persistent device TLS identity, private activation token,
  device-specific setup AP, physical setup card and AP-bound owner enrollment.
- Setup: constrained hostname application, Home Wi-Fi with bounded attempt and
  setup-AP restore, and an onboarding-completion record.
- Orientation: a changed rotation is previewed and kept only when the owner
  confirms on the panel or setup page within 30 seconds; otherwise it is rolled
  back. Unchanged saves skip the preview, and boot rolls back an interrupted
  preview.
- Touch: calibration starts from the panel's Wi-Fi menu or the setup page. It
  uses five targets, a verification tap and a 30-second rollback. The result is
  applied through the X input matrix together with rotation and the X server's
  own matrix. A browser reset re-applies within a second.
- Pairing: `rapid-pi` is the only credential-issuing endpoint (TLS, port 8003).
  Approval works from the Pi panel or the setup page. The Windows companion's
  `--pairing-url` client decrypts the X25519/HKDF/AES-256-GCM envelope, stores
  the key with DPAPI and checks the pinned Pi identity before reconnecting.

**Tested:** on 2026-09-15 the WSL x86-64 Release build passed 11/11 CTests in
35.28 s, along with the dashboard graph regression, a setup-page script parse
and an offscreen QML load. The amd64 package passed `CheckPackage.cmake`. Not
run locally: Debug, ARM64.

**Verified (hosted CI):** `358f26f`, run `35023662989` (Windows companion, Linux
Debug and Release). The calibration and orientation commits have not been
through hosted CI.

**Deployed:** the development Pi runs the older package from `7b5e910`. None of
the work above is installed there. No image has been assembled or flashed.

**Accepted:** nothing. Every open issue remains open against its full criteria;
#7 has its source fix but still needs real AC1/ACC evidence.

## Next

Software gaps that need no hardware:

1. #10 — companion acquisition. The setup page offers no companion download,
   and pairing still needs a command line with the address and certificate
   fingerprint. There is no discovery or manual-address UI.
2. Retire the remaining active v3 companion transport (ACC's broadcast API stays).

Hardware: for #9, touch device naming under evdev, tap accuracy in both
orientations and persistence across reboot. For #11, orientation rollback on
the real panel plus the failed Wi-Fi, interrupted onboarding and bad calibration
drills (`wiki/Release-Acceptance.md` §2 and §4). Run the full release acceptance
before any v1.0 claim.

## Device and workspace

- Reach the Pi with `ssh rapid@rapid`. Check live services and confirm the
  recorder is idle first. Keep keys, configuration, the database and recordings
  in timestamped `.old` backups.
- Local Linux builds: `developer/WSL-BUILD.md`. The full local gate is
  `developer/Test-LocalCandidate.ps1`.
- Session evidence goes in ignored `.local/sessions/YYYY-MM-DD-topic/`.
