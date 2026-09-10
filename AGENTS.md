# Working in raPId

Read HANDOFF.md, then initialize the wiki submodule and read wiki/Resume-Work.md.
The repository and its pinned wiki are sufficient to resume source development.

- Runtime work belongs in C++. Inspect current code and service state; old logs
  are historical evidence, not current deployment instructions.
- Update relevant wiki guides, Development-Log and Resume-Work when behavior,
  verification or pending work changes. Keep HANDOFF.md concise.
- Commit/push wiki changes first, then commit the wiki pointer in this repository.
  Follow wiki/Documentation-Workflow.md; preserve uncommitted work in both repos.
- Keep session outputs under ignored .local/sessions/YYYY-MM-DD-topic/.
  Never publish device credentials, paired keys, recordings or raw private logs.
- Preserve recovery copies outside tracked source. Deploy only while recording
  is idle and record checks plus rollback location in the session evidence.
- Build/tests: cpp/CMakeLists.txt and .github/workflows/verify.yml. Enable
  RAPID_BUILD_QT_DISPLAY for Qt tests. Use isolated fixtures for synthetic telemetry.
- The old developer directory is optional local history, not a source dependency.
  Existing installed companion paths/private kits can still depend on it;
  relocate those deliberately before removing it.
