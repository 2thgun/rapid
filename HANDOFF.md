# Current handoff

Published source through `f5fd25b` adds first-boot AP onboarding, physical
activation-token display, browser owner enrollment, constrained hostname
application and protected Home Wi-Fi onboarding. The root Wi-Fi service manages
only `rapid-home`, bounds its connection attempt and restores the setup AP on
failure. Native test execution and hardware acceptance remain pending. Rotation,
calibration and the full pairing flow are still unapplied. Do not deploy this
work yet.
The documentation audit checkpoint `b5a199e` passed hosted verification in
run `34716780316`. All open issues remain open against their full acceptance
criteria; #7 has its source fix but still requires real AC1/ACC evidence.

Source checkpoint before the current unpublished-on-device work: `6d3ee81`. Opt-in browser owner enrollment is implemented
in the loopback C++ setup service. A private activation token authorizes the
one-time owner claim; settings saves still record desired values only.
AP provisioning, physical token display and applied Wi-Fi have since been
implemented in source but remain unverified on hardware. Display settings,
calibration and companion pairing remain incomplete.

Verification: all nine isolated Pi Release CTests passed, including HTTP
enrollment/login/settings; browser syntax and graph regressions passed.
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
DPAPI storage, physical approval, live key reload or verified revocation yet.

The next unpublished source change keeps paired-key namespaces strictly inside
the replay database. Dashboard and recording session IDs remain the raw 32-digit
wire ID, avoiding a derived key fingerprint in application-visible state.

Next: implement pairing records/approval and Windows handshake, then display
rotation/calibration recovery. Complete the fresh-card and real AC1/ACC/MoTeC checks
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
