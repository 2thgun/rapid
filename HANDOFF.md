# Current handoff

Active work is the flash/setup/pairing flow documented in
`wiki/Provisioning.md`. The C++ setup service persists device identity, owner
login and desired hostname/orientation. The package now includes the Home/AP/Off
network worker and defaults to Home with automatic AP recovery. Applying device
settings, HTTPS/AP bootstrap and pairing UI remain incomplete.

Current source `194e8a1` passed hosted Windows/Linux verification, the local
Windows Zig companion self-test, browser graph test, all eight Pi CTests, ARM64
package inspection and the image guard checks. The candidate package SHA-256 is
`5100f3412cb410d6402ec963dccb5cfd2664568973a9c63e2bcd46c10925cb26`.
The image build deliberately remains blocked until the package contains the
`rapid-image-ready-v1` contract marker; no image was assembled or flashed.

The Pi still runs the older `7b5e910` preview package. This candidate was built
and tested only in `/home/rapid/.local/issue-fixes-final`; it was not deployed.
Fresh installation is tracked in issue #1, and reboot, touch and live AC1/ACC
acceptance remain pending in issue #6.

Runtime/UI checkpoint: `bd92a46` (2026-09-09). The Qt repair is deployed.
Documentation now lives in the pinned GitHub wiki submodule.

Hosted verification for `194e8a1` passed in Actions run `34714482247`.

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
- Next: deploy and run the hardware acceptance guide; complete first-boot setup,
  pairing and recovery; then produce and flash the release image.
- Device access uses `ssh rapid@rapid`. Inspect current service state before
  deployment. Private credentials, keys and recordings are deliberately outside Git.
- Development does not depend on the old developer folder. Use ignored
  `.local/sessions/` for temporary outputs and preserve private installed
  kits/keys separately before retiring old folders.

Keep this file brief. Durable history belongs in the wiki; deployment claims must
include actual checks rather than relying on old conversation notes.
