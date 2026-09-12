# Current handoff

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
