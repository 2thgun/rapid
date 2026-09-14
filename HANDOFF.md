# Current handoff

Published source through `a694da8` adds first-boot AP onboarding, physical
activation-token display, browser owner enrollment, constrained hostname
application and protected Home Wi-Fi onboarding. The root Wi-Fi service manages
only `rapid-home`, bounds its connection attempt and restores the setup AP on
failure. The ARM64 native Release suite and package verifier pass; hardware
acceptance remains pending. Rotation,
calibration and the full pairing flow are still unapplied. Do not deploy this
work yet.
The latest wiki verification record is `17ed821`.
The documentation audit checkpoint `b5a199e` passed hosted verification in
run `34716780316`. All open issues remain open against their full acceptance
criteria; #7 has its source fix but still requires real AC1/ACC evidence.

Source checkpoint before the current unpublished-on-device work: `6d3ee81`. Opt-in browser owner enrollment is implemented
in the loopback C++ setup service. A private activation token authorizes the
one-time owner claim; settings saves still record desired values only.
AP provisioning, physical token display and applied Wi-Fi have since been
implemented in source but remain unverified on hardware. Display settings,
calibration and companion pairing remain incomplete.

Verification: all ten isolated Pi Release CTests passed, including HTTP
enrollment/login/settings, v4, pairing, network delivery and recorder recovery;
the generated ARM64 preview package passed its contents/dependency/conffile
verifier. Browser syntax and graph regressions passed.
Hosted Windows companion/MSI and Linux Debug/Release verification passed:
https://github.com/2thgun/rapid/actions/runs/34716119927

Last recorded deployment: `7b5e910` on the development Pi. The newer enrollment
and Wi-Fi failure-reporting changes have not been deployed. The older ARM64
candidate package from `194e8a1` does not contain those changes; see
`wiki/Validation-Status.md` for artifact provenance. No image has been assembled
or flashed; the release marker deliberately remains absent.

The receiver accepts up to 16 configured v4 keys and keeps their persisted
watermarks separate. Private setup state now stores up to 16 paired-PC records;
the owner can list key-free labels and revoke a record through the authenticated
setup page. Runtime keys are loaded from that private store at startup. This is
not a completed pairing flow: there is no graphical approved handshake, Windows
DPAPI storage or physical approval yet. For an existing paired store, the Pi
reloads its private key records at most one second after a change; removing the
last record rejects packets rather than restoring a legacy shared key.

The paired-replay source change keeps key namespaces strictly inside the replay
database. Dashboard and recording session IDs remain the raw 32-digit wire ID,
avoiding a derived key fingerprint in application-visible state.

An internal pairing-window state machine now creates random transaction/nonce
pairs and derives deterministic eight-digit comparison codes. It enforces one
pending request, a two-minute window, cancellation, five retries against the
same request and one-time approval consumption. It is not an HTTP endpoint or a customer pairing flow:
certificate transport, X25519/AES envelope, panel approval and Windows DPAPI
storage still remain.

The package verifier also inspects the installed first-boot, AP-provisioning and
setup service definitions. It rejects a package unless private setup state is
created by `rapid`, only the NetworkManager provisioner runs as root, and the
setup listener/token paths match the fixed bootstrap contract.
The verifier accepts an explicit `DPKG_DEB` path for its test harness while
retaining `dpkg-deb` as the real package-build default.

Demo review, 2026-09-13: the companion saved one 12.5-minute ACC recording
while the Pi split it into five finalized bundles after brief packet silences.
The Pi retained and published received telemetry; this was recorder
fragmentation, not loss of the final laps. Published source keeps the
dashboard's 1.5-second disconnect indication but retains the spool and session
identity for ten seconds, unless an explicit non-driving status arrives. The
same release also reports queued settings application and preserves v4 wire
session IDs while idle. The live Pi remains untouched. The Codex sandbox account
was denied access to `\\rapid\Telemetry` under its `valid users = rapid` Samba
configuration, so validate the user's Explorer credentials and refresh behavior
before claiming network-folder acceptance.

The published peer-store edit provides idempotent reconnect records and
non-finite timestamp rejection. An exact-path ARM64 Release rebuild passed all
ten CTests in 16.83 seconds. This store helper still needs production pairing
integration before it represents a complete reconnect flow. Pairing-window approval comparison is now
constant-time; the corrected ARM64 gate passed all ten tests in 16.74 seconds.
Paired-key mode also rejects v3 packets when its active key set is empty, so
revoking the last remembered PC cannot reopen unauthenticated telemetry. That
fix is documented in wiki commit `5e6b471` and verified by the 16.92-second
ARM64 ten-test gate.
Settings status now reports an apply queue only while its protected request
file exists; the empty, queued and consumed states are covered by the setup
authentication regression. The ARM64 ten-test gate passed in 16.97 seconds;
this does not apply orientation or calibration. An authenticated owner can
retry a failed hostname application through `POST /api/v1/settings/retry` using
the current revision; it preserves saved settings and queues no new revision.
The UI and route regression passed the exact ARM64 ten-test gate in 17.03
seconds. Wiki evidence is `45f0ba2`.
The pairing library now also provides a tested X25519/HKDF-SHA-256/AES-256-GCM
key envelope primitive with transaction-bound authenticated data. It is not
yet wired to HTTP, TLS, the panel or the Windows companion. The new coordinator
consumes an approved request, stores its generated telemetry key privately and
returns the envelope once; this core integration is covered by the 16.80-second
ARM64 gate. See wiki evidence `dc2524e`.

The Windows daemon now has a per-user DPAPI credential container and explicit
load/store options; it also has a WinHTTP HTTPS setup probe that verifies an
explicit SHA-256 certificate pin and fails closed on mismatch. The Pi panel
now renders and approves the pairing handoff; the Windows request, envelope
decryption and reconnect client remain pending. The Pi package now includes
`rapid-display-recovery` with preview/confirm/rollback, boot recovery and stale
calibration reset; its output and evdev paths still need hardware confirmation.

Ubuntu 24.04 under WSL2 is now the local Linux build path; `developer/WSL-BUILD.md`
records the reproducible setup. The local x86-64 Release build and all eleven
CTest targets passed, along with the browser graph regression. The setup server
uses TLS in the packaged service, and first boot provisions a persistent
self-signed device certificate/key; the network regression covers a real
HTTPS request. SetupAuth exposes pairing transitions over HTTPS, and the owner
setup page can open/cancel the window and approve the displayed code. Plain HTTP
does not expose those pairing routes. The coordinator now publishes only
transaction, label, code and expiry metadata to `/run/rapid/pairing.json` with
private permissions, clearing it on expiry, cancellation or consumption.
Its state transitions are serialized across panel, browser and companion
polling.
Physical panel rendering and touch approval are implemented in the Qt display;
the Windows pairing request/envelope/DPAPI client flow and pinned reconnect
remain pending, while the Pi ARM64 rebuild and hardware flow are still required.

Latest local checkpoint: source `29d22f4` and wiki `b35cae5`. On 2026-09-14
the pinned Zig Windows build and native self-test passed, including CNG
Curve25519 agreement, HKDF-SHA-256, AES-256-GCM decrypt, and tamper rejection,
alongside the AC1/Content Manager adapter fixture checks. The self-test was
run with an explicit temporary output directory; the default repository-root
fixture location is not writable under the local sandbox.

Next: complete the Windows pairing request/envelope client and pinned
reconnect. Complete the fresh-card
and real AC1/ACC/MoTeC checks
in `wiki/Release-Acceptance.md` before claiming v1.0.

Initialize pinned documentation with `git submodule update --init --recursive`,
then read `wiki/Resume-Work.md` and `wiki/Development-Log.md`.
The native C++ runtime and Windows companion use v4; Qt provides five pages.
Remaining active v3 compatibility is tracked for retirement during pairing work.

Use `ssh rapid@rapid`; inspect live services before deployment. Preserve private
keys, configuration, databases and recordings with a timestamped `.old` backup.
Temporary evidence belongs in ignored `.local/sessions/`. Development resumes
from this repository and wiki; old developer folders are optional history,
though installed private kits may still reference them.



