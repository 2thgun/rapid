# PROJECT.md: how work is done in raPId

This document sets the working method for anyone who changes raPId, whether a
person or an agent. The product itself is documented in the
[wiki](https://github.com/2thgun/rapid/wiki).

## 1. Where things live

| What | Where |
| --- | --- |
| Source, tests, CI | this repository |
| The manual: how raPId works and how to use, build and test it | the wiki, pinned here as the `wiki/` submodule |
| User-visible changes | `CHANGELOG.md` |
| Open work and its acceptance criteria | GitHub issues and milestones |
| Planning, task briefs, session logs, device evidence | the maintainer's local workspace, **outside Git** |

The wiki describes **the current behavior only**. It doesn't hold status
reports, session logs, handoffs, "resume work" notes or audit trails. Git
history and issues keep that record. When behavior changes, change the page
that describes it. Don't append a note about the change.

## 2. Claim vocabulary

Use these words exactly, in commits, pull requests and issues:

| Term | Means | Evidence required |
| --- | --- | --- |
| **Implemented** | The code exists and compiles. | A revision. |
| **Tested** | Automated checks covering it pass. | Named targets, result, build type. |
| **Verified** | The full gate passed at a named revision. | Revision, configuration, test count, where it ran, and the CI link. |
| **Deployed** | It is installed on a real device. | Device, artifact, revision, and where the rollback copy is. |
| **Accepted** | A person saw the real behavior on real hardware. | The checklist row, the date, the tester, and what was observed. |

A passing test suite is not acceptance. Synthetic fixtures, screenshots and
injected input are not physical evidence. An HTTP health check doesn't prove
UDP telemetry arrived, and a sent-packet counter on the PC doesn't prove the
Pi accepted anything. When something hasn't been run, write `not run`.

## 3. Working an issue

- An issue's acceptance criteria are the contract. Close an issue only when
  every row has evidence, not when the code change lands.
- Write tests from the required behavior, not from the current implementation.
- **Nothing ships without its producer.** An endpoint or UI that acts on data
  nothing writes yet is not implemented.
- **Cross-platform protocols need cross-platform fixtures.** Anything the
  companion sends and the Pi parses (v4 telemetry, pairing) is tested with
  fixtures produced by the real code on the other platform.
- If a gate fails, keep the evidence and leave the issue open. Never weaken a
  test to get a pass.

## 4. Branches and commits

- Use one focused branch per change, based on the current `origin/main`, and
  merge through a pull request with green CI. Never push directly to `main`, and
  never force-push shared branches.
- Stage files explicitly. Never `git add -A` or `git commit -a`; other work is
  often in progress in the same tree.
- **No AI attribution.** Don't add `Co-Authored-By` trailers for any AI tool, or
  "Generated with …" lines, to commits, pull requests or issues.
- Runtime behavior belongs in C++.

## 5. Documentation

With each change:

1. Update the wiki page that describes the behavior you changed. Keep pages
   accurate, not historical.
2. Add a line to `CHANGELOG.md` if users will notice the change.
3. The wiki is its own repository. Commit and push the wiki first, then commit
   the updated `wiki` submodule pointer with the source change:

```sh
git -C wiki switch master && git -C wiki pull --ff-only
# edit wiki pages
git -C wiki add <pages> && git -C wiki commit -m "docs: ..." && git -C wiki push
git add wiki <source files> && git commit
```

A GitHub ZIP download doesn't include the wiki. Clone with
`--recurse-submodules`.

## 6. The verification gate

Before you claim anything stronger than "implemented", run the gate:

- Linux, **clean** Debug **and** Release builds with
  `RAPID_BUILD_LOG_STATUS=ON`, `RAPID_BUILD_QT_DISPLAY=ON` and
  `RAPID_BUILD_PI_PACKAGE=ON`. Every CTest target must pass.
- `node cpp/display/tests/qt_display_tests.js`.
- The Windows companion build and `--self-test`.
- An ARM64 build before any claim about deployment. An x86-64 pass says nothing
  about the Pi's compiler, `-Werror` or dependencies.

Rules learned from false results:

- Build from an empty build directory. Reused object files have produced wrong
  passes.
- The log is the evidence. A result counts only if the log shows the command,
  its exit code and the CTest summary line.
- Hosted CI is the source of truth. Check it before building on a commit.

The commands are in [Testing](https://github.com/2thgun/rapid/wiki/Testing).

## 7. Privacy

Never commit or publish device credentials, paired or telemetry keys, TLS
private keys, recordings, private Wi-Fi names or raw system logs, whether in
source, the wiki, issues or pull requests. Put local scratch output in the
ignored `.local/` directory. Before committing, review both the source and the
wiki diffs for secrets.

## 8. Device and safety rules

The development Pi is a single shared device.

- Only one person or agent works on the Pi at a time. Coordinate before any SSH
  session, deployment, reboot or test drive.
- Make sure the recorder is idle before you restart, deploy or power down.
- Before an upgrade, back up `/var/lib/rapid-setup`, `/var/lib/rapid` and
  `/etc/rapid` with a timestamp, and note where the backup is. Deleting the
  state database deletes replay history. Never restore old data over newer
  data without reconciling it first.
- Never clone a configured card to another unit. Each device generates its own
  identity.
- **Device-facing work isn't done until it has run on the Pi.** That covers the
  display, rotation, touch, network, boot, recording and telemetry. Until then
  it is at most "tested".
- **Probe before you design.** Before you build on a device tool or interface
  (evdev naming, NetworkManager behavior, Xorg), check read-only on the real Pi
  that it does what the design assumes.

## 9. Definition of done

- [ ] Runtime behavior is in C++.
- [ ] Tests exist and come from the acceptance criteria.
- [ ] Clean Debug and Release gates pass. The log shows the exit codes and
      test summaries.
- [ ] Pushed, with hosted CI green.
- [ ] Device-facing changes were exercised on the Pi, or the pull request says
      plainly that they weren't.
- [ ] The result is described with the §2 vocabulary.
- [ ] The affected wiki pages describe the new behavior. `CHANGELOG.md` is
      updated if the change is user-visible.
- [ ] The wiki is pushed before the submodule pointer is committed.
- [ ] Both diffs were reviewed for secrets.
- [ ] The issue records evidence, and it is closed only if every acceptance row
      passed.
