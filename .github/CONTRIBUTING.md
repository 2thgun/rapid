# Contributing

Clone with the wiki: `git clone --recurse-submodules https://github.com/2thgun/rapid.git`.
Build and test commands are in [Building](https://github.com/2thgun/rapid/wiki/Building)
and [Testing](https://github.com/2thgun/rapid/wiki/Testing).

- Work on a branch, open a pull request, and wait for green CI. Don't push to `main`.
- Stage files explicitly. Don't use `git add -A`.
- No AI attribution in commits, PRs or issues.
- Runtime code is C++.
- Before calling something done, pass clean Debug and Release builds, the Qt check and the companion self-test.
- Device-facing changes (display, touch, network, boot, recording) aren't done until they've run on a real Pi. If yours hasn't, say so in the PR.
- The wiki describes how things work now. Update the page you changed and don't add logs or status notes. Push the wiki before committing the submodule pointer.
- Add a `CHANGELOG.md` line for user-visible changes.
- Never commit keys, passwords, recordings or raw logs.
