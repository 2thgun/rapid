# Current handoff

Active work: flash/setup/pairing, documented in `wiki/Provisioning.md`.
First slice adds opt-in C++ device identity/configuration persistence and
read-only `/api/v1/setup`. A separate loopback C++ setup server adds owner login;
CPack assembles a preview Pi application package. See `wiki/Pi-Package.md`.
The preview package is deployed; first-boot provisioning is not implemented.
Next: HTTPS/AP bootstrap and the privileged
service boundary. See wiki log
for verification; do not describe the full onboarding flow as implemented.
Current source `7b5e910` passed all eight Pi CTests, ARM64 package inspection and
extracted-package integration. Hosted Windows/Linux Debug/Release and amd64
packaging passed (Actions `34473689994`). ARM64 preview is in ignored `dist/`
and installed on the Pi with key/database preservation in a timestamped `.old`
backup. Evidence is in `.local/sessions/2026-09-10-setup-auth/` and
`.local/sessions/2026-09-10-package-deploy/`. The image
profile validates, but first-boot AP provisioning is still required; no image
has been written. The build guard was checked and returns exit 2 for missing
first-boot provisioning. Fresh installation is tracked in GitHub issue #1;
reboot, touch and live AC1/ACC acceptance remain pending in issue #6.

Runtime/UI checkpoint: `bd92a46` (2026-09-09). The Qt repair is deployed.
Documentation now lives in the pinned GitHub wiki submodule.

CI follow-up: `973dbee` fixes network-test isolation after repeated hosted
Linux failures. Check the latest GitHub Actions conclusion before describing a
revision as verified; local/Pi test results alone are insufficient.

```sh
git submodule update --init --recursive
```

Then read [Resume work](https://github.com/2thgun/rapid/wiki/Resume-Work) or
`wiki/Resume-Work.md`, and the [development log](https://github.com/2thgun/rapid/wiki/Development-Log).

- Native C++ Pi runtime and Windows companion; production telemetry uses v4.
- Qt is the active five-page panel. Stacked graphs and larger touch controls were
  checked on-device; Home/AP/Off selection is implemented, with Home at startup.
- Eight CTests including the Qt model passed for the deployed package source. Synthetic graph
  traces/gaps and screenshots were verified. Live AC1/CM/MoTeC rehearsal,
  physical finger calibration and a fresh Qt cold boot remain open.
- Next: demo acceptance; wheel-speed unit audit; complete active v3 transport
  retirement without changing ACC's distinct broadcast API; reproducible Pi setup.
- Device access uses `ssh rapid@rapid`. Inspect current service state before
  deployment. Private credentials, keys and recordings are deliberately outside Git.
- Development does not depend on the old developer folder. Use ignored
  `.local/sessions/` for temporary outputs and preserve private installed
  kits/keys separately before retiring old folders.

Keep this file brief. Durable history belongs in the wiki; deployment claims must
include actual checks rather than relying on old conversation notes.
