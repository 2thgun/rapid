# Current handoff

Runtime/UI checkpoint: `bd92a46` (2026-09-09). The Qt repair is deployed.
Documentation now lives in the pinned GitHub wiki submodule.

```sh
git submodule update --init --recursive
```

Then read [Resume work](https://github.com/2thgun/rapid/wiki/Resume-Work) or
`wiki/Resume-Work.md`, and the [development log](https://github.com/2thgun/rapid/wiki/Development-Log).

- Native C++ Pi runtime and Windows companion; production telemetry uses v4.
- Qt is the active five-page panel. Stacked graphs and larger touch controls were
  checked on-device; Home/AP/Off selection is implemented, with Home at startup.
- Five native CTests plus the Qt model test passed on the Pi. Synthetic graph
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
