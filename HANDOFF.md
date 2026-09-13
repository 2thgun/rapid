# Current handoff

Active source work on 2026-09-13: first-boot AP onboarding is uncommitted.
The C++ initializer persists device-specific AP credentials and an activation
token; the constrained root provisioner creates the fixed NetworkManager AP
profile before starting the AP-bound setup server. The Qt panel renders the
private local bootstrap details. Native test execution and hardware acceptance
remain pending. Authenticated settings now queue constrained hostname application;
Wi-Fi, rotation, calibration and pairing are still unapplied. Do not deploy this
work yet.
The documentation audit checkpoint `b5a199e` passed hosted verification in
run `34716780316`. All open issues remain open against their full acceptance
criteria; #7 has its source fix but still requires real AC1/ACC evidence.

Source checkpoint: `6d3ee81`. Opt-in browser owner enrollment is implemented
in the loopback C++ setup service. A private activation token authorizes the
one-time owner claim; settings saves still record desired values only.
AP provisioning, physical token display, applied Wi-Fi/display settings,
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

Next: connect first-boot AP provisioning and physical activation-token display
to browser enrollment, then implement settings application, pairing and
keyboardless recovery. Complete the fresh-card and real AC1/ACC/MoTeC checks
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
