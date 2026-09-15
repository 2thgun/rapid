# Current handoff

Read [PROJECT.md](PROJECT.md) for the working method and claim vocabulary.
History lives in `wiki/Development-Log.md`; this file only describes now.

## Checkpoint — 2026-09-15

Functional source checkpoint: `5bdfc6c` (first-boot pairing runtime).
Later commits on `main` are repository and documentation cleanup only.

**Implemented, not hardware-verified:**

- First boot: persistent device TLS identity, private activation token,
  device-specific setup AP, physical setup card and AP-bound owner enrollment.
- Setup: constrained hostname application, Home Wi-Fi with bounded attempt and
  setup-AP restore, display rotation through `rapid-display-recovery`
  (preview, confirm, boot-time rollback), touchscreen calibration file reset,
  and an onboarding-completion record.
- Pairing: `rapid-pi` is the only credential-issuing endpoint (TLS, port 8003).
  Approval works from the Pi panel or the setup page. The Windows companion's
  `--pairing-url` client decrypts the X25519/HKDF/AES-256-GCM envelope, stores
  the key with DPAPI and checks the pinned Pi identity before reconnecting.

**Tested:** on 2026-09-15 at `5bdfc6c`, the WSL x86-64 Release build passed
11/11 CTests in 26.31 s plus the dashboard graph regression. Not run in that
pass: Debug, Windows companion self-test, package verifier, ARM64.

**Verified (hosted CI):** last at `b5a199e`, run `34716780316`. The newer local
commits have not been through hosted CI.

**Deployed:** the development Pi runs the older package from `7b5e910`. None of
the work above is installed there. No image has been assembled or flashed.

**Accepted:** nothing. Every open issue remains open against its full criteria;
#7 has its source fix but still needs real AC1/ACC evidence.

## Next

Software gaps that need no hardware:

1. #9 — touchscreen calibration capture. Only reset exists
   (`/api/v1/calibration/reset`); nothing records a calibration, and
   `rapid-apply` still reports calibration as pending.
2. #10 — companion acquisition. The setup page offers no companion download,
   and pairing still needs a command line with the address and certificate
   fingerprint. There is no discovery or manual-address UI.
3. Retire the remaining active v3 companion transport (ACC's broadcast API stays).

After that, run `wiki/Release-Acceptance.md` on hardware before any v1.0 claim.

## Device and workspace

- Reach the Pi with `ssh rapid@rapid`. Check live services and confirm the
  recorder is idle first. Keep keys, configuration, the database and recordings
  in timestamped `.old` backups.
- Local Linux builds: `developer/WSL-BUILD.md`. The full local gate is
  `developer/Test-LocalCandidate.ps1`.
- Session evidence goes in ignored `.local/sessions/YYYY-MM-DD-topic/`.
