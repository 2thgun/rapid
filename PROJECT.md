# PROJECT.md — how work is done in raPId

This is the entry point for anyone working in this repository, human or
agent. It defines the working method: what to read, what words mean, what
counts as evidence, where things get written down, and how several people or
agents work at once without overwriting each other.

It deliberately does not restate the rules that other files already own.
Duplicated rules drift and then contributors follow different copies. Where
this document points at another file, that file is authoritative.

| Question | Authoritative file |
| --- | --- |
| Repository working conventions, short form | `AGENTS.md` |
| Current checkpoint and next task | `HANDOFF.md` |
| Which doc is canonical, and commit order | `wiki/Documentation-Workflow.md` |
| Engineering state, deeper than the handoff | `wiki/Resume-Work.md` |
| What has actually been verified, and when | `wiki/Validation-Status.md` |
| Build and test commands | `wiki/Testing.md` |
| Hardware and release sign-off | `wiki/Hardware-Acceptance.md`, `wiki/Release-Acceptance.md` |
| Everything else | the wiki sidebar |

## 1. Read order at the start of every session

Do this before proposing or changing anything. It takes a few minutes and
prevents the most common failure in this project, which is acting on a stale
description of the device.

```sh
git status --short
git submodule update --init --recursive
```

1. `HANDOFF.md` — the current checkpoint and the next task.
2. `wiki/Resume-Work.md` — the engineering view of the same moment.
3. `wiki/Validation-Status.md` — what has been verified, and against which
   revision.
4. The issue you intend to work, in full, including its acceptance criteria.

Then inspect the actual state you are about to change: the current source,
the current service state on the device, the current package contents. Old
log entries are dated evidence of what was true once. They are not a
description of the system now, and they are never deployment instructions.

## 2. Claim vocabulary

Most confusion in this project comes from one word doing five jobs. Use these
terms precisely, in commits, logs, issues and conversation. If you cannot
honestly use a stronger word, use a weaker one.

| Term | Means | Evidence required |
| --- | --- | --- |
| **Implemented** | The code exists and compiles. | A revision. Nothing more is claimed. |
| **Tested** | Automated checks covering it pass locally. | Named test targets, the result, and the build type. |
| **Verified** | The full suite passed at a named revision, in a named configuration. | Revision, configuration, targets, duration, and where it ran. Hosted CI runs are linked. |
| **Deployed** | Installed on a real device. | Which device, which artifact, which revision, and where the rollback copy is. |
| **Accepted** | A human observed the real behavior on real hardware, against a runbook. | The runbook row, the date, the operator, and the observed result. |

Rules that follow from this:

- A passing test suite is not acceptance. Synthetic fixtures, screenshots and
  injected input events are not physical evidence.
- An HTTP health response does not prove UDP telemetry. A sent-packet counter
  on the PC does not prove the Pi accepted anything. Use receiver counters.
- "It should work" is not a status. Write `not run` rather than inferring a
  pass.
- Implemented and unverified is a perfectly respectable state. Say it plainly
  rather than rounding it up.

## 3. Document before you act, confirm after

Every step that changes state — an edit, a command, a build, a deployment, a
git operation — is written down before it is taken and confirmed after it
completes. State what you are about to do, why, and what you expect. Then
state what actually happened, including when it differs from the expectation.

This is not ceremony. It is what makes a session reconstructible by the next
contributor, and it is what lets several agents share one repository and one
physical device without guessing at each other's intent.

A step that was taken but not written down is treated as an unknown change
and should be re-verified.

## 4. During the session

Work in the smallest scope that satisfies the task. Runtime work belongs in
C++. Keep disposable output in an ignored session directory:

```
.local/sessions/YYYY-MM-DD-topic/
```

One directory per session per topic, named for the work, not the person. Put
logs, screenshots, diagnostic excerpts and measurements there. Never put
credentials, paired keys, recordings, private Wi-Fi names or raw system logs
in the repository or the wiki — those stay in `.local/private/` or off the
machine entirely.

Preserve a rollback copy outside tracked source before changing anything on a
device, and record where it is.

## 5. Finishing a session

The order matters, because the wiki is a separate repository pinned here as a
submodule. Full detail is in `wiki/Documentation-Workflow.md`; the shape is:

1. Append a dated entry to `wiki/Development-Log.md`.
2. Update the relevant wiki guide if behavior or instructions changed.
3. Update `wiki/Resume-Work.md` and `HANDOFF.md` only if the current
   checkpoint actually moved.
4. Update `CHANGELOG.md` if the change is user-visible.
5. Review both diffs for secrets and private details.
6. Commit and push the wiki first, then commit the submodule pointer together
   with the source change.

Never commit a source change that describes wiki content which has not been
pushed. The pointer must always resolve for someone cloning fresh.

### Development-Log entry template

Consistent shape matters more than prose quality. Every entry answers the
same questions in the same order, so entries can be compared and skimmed.

```markdown
## YYYY-MM-DD — short topic

**Changed.** What was modified, and in which components.

**Why.** The issue or defect this serves, linked.

**Verification.** Which targets ran, in which configuration, at which
revision, with results and duration. Link hosted runs. Name what was not
run.

**Deployment.** Whether anything was installed anywhere, which artifact,
and where the rollback copy lives. "Not deployed" is a valid and common
answer.

**Outstanding.** What this deliberately did not do, and what remains
before the owning issue can close.
```

### HANDOFF.md rules

`HANDOFF.md` is read first by everyone, so it stays short and current. It
describes the present checkpoint and the immediate next task. It is not a
history — the log is the history. When the handoff and a log entry disagree,
the handoff is wrong and should be corrected, because the log is dated and
the handoff claims to be now.

Only the contributor finishing a session updates it, and only when the
checkpoint actually changed.

## 6. The verification gate

Before claiming anything stronger than "implemented", run the suite. Commands
and dependencies are in `wiki/Testing.md` and `wiki/Operations.md`; the shape
is a Debug pass and a Release pass on Linux with the optional targets
enabled:

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug \
  -DRAPID_BUILD_LOG_STATUS=ON -DRAPID_BUILD_QT_DISPLAY=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Report the number of targets that ran rather than quoting a count from an
older document — the count grows as suites are added, and stale counts in
prose are a recurring source of confusion. The browser regression
(`node cpp/tests/dashboard_graph_tests.js`) and the Windows companion
self-test are part of the gate, not extras.

Package inspection is not installation. Installation is not acceptance.

## 7. Working an issue

Issues here carry full acceptance criteria, and those criteria are the
contract. Source correctness is usually only one row of several.

- Do not close an issue because its code change landed. Close it when every
  acceptance row has recorded evidence.
- If the work splits, narrow the issue explicitly and say what was moved
  where, rather than closing it and opening a vaguer one.
- If something is already satisfied by existing code, say so with the file
  and line as evidence, rather than reimplementing it.
- Prerequisite issues closing does not pass the gate that depends on them.

When a gate fails, preserve the evidence, name the failing row, and leave the
issue open. Do not downgrade the test to obtain a pass.

## 8. Device and safety rules

The Raspberry Pi is a single shared physical resource and the demo kits carry
private keys. Treat both accordingly.

- Confirm the recorder is idle before restarting, deploying or powering down.
- Preserve keys, configuration, the state database and recordings across any
  upgrade, with a timestamped `.old` copy, and record its location.
- Deleting the state database deletes replay history. Reconcile data
  deliberately while services are stopped, never by restoring old paths over
  newer data.
- Do not clone a configured card for another unit. A new device generates its
  own identity, credentials and keys.
- Never publish device credentials, paired keys, recordings or raw private
  logs, in either repository.

### Known traps

These have each cost someone a session already.

- `systemd/` in this repository is the **old source-tree** unit set. The
  packaged device uses the units from `packaging/`, running binaries under
  `/usr/lib/rapid`. Installing the former over the latter silently runs the
  old tree.
- The `developer/` folder outside this repository is optional local history,
  not a source dependency — but installed companion kits may still point into
  it. Relocate deliberately before removing anything.
- An initialized submodule can be on a detached HEAD. Switch to the branch
  and fast-forward before editing the wiki; never reset away local changes.
- A GitHub ZIP download omits submodule contents. Documentation work needs a
  recursive clone.

## 9. Working alongside other contributors and agents

Several agents in one repository, against one device, need three things:
declared scope, separate scratch, and append-only history.

**Declare scope before starting.** State which issue or component you are
taking and what you intend to touch. Two contributors in the same source
files at once is a merge problem; two contributors on the device at once is a
correctness problem.

**The device is exclusive.** Announce deployment intent before touching the
Pi, and confirm when you are off it. Never deploy while another session's
work is installed but unverified, and never while a recording is active.

**Keep scratch separate.** Each session gets its own
`.local/sessions/YYYY-MM-DD-topic/`. Do not write into another session's
directory or edit its files.

**Treat history as append-only.** Add a new dated entry to the development
log; do not rewrite someone else's. If an earlier entry was wrong, add a
correction that says so and links it, which is how `wiki/Wiki-Audit.md`
already works.

**HANDOFF.md is the hot spot.** It is small, everyone edits it, and it
conflicts easily. Only the finishing contributor updates it, it stays brief,
and it describes only the current checkpoint.

**Pull before you write documentation.** The wiki advances independently,
including from edits made on the GitHub website. Fast-forward first.

**Hand off in writing.** An unfinished task is handed over with what was
done, what was verified, what is deliberately incomplete, and where the
evidence is. Anything else forces the next contributor to rediscover the
state, which is the expensive part.

## 10. Definition of done

A change is done when all of these are true. If one is not, say which.

- [ ] The change is in C++ if it is runtime behavior.
- [ ] Tests covering it exist and the suite passes in Debug and Release.
- [ ] The result is described with the correct claim vocabulary from §2.
- [ ] A dated development-log entry records what ran and what did not.
- [ ] Affected guides are updated; `CHANGELOG.md` is updated if user-visible.
- [ ] `HANDOFF.md` and `Resume-Work.md` are current, or deliberately
      unchanged because the checkpoint did not move.
- [ ] Wiki is pushed, then the pointer is committed with the source change.
- [ ] Both diffs were reviewed for secrets.
- [ ] The owning issue records evidence, and is closed only if every
      acceptance row is satisfied.
- [ ] Every step taken was written down before it was taken and confirmed
      after.
