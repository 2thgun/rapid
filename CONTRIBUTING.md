# Contributing to raPId

Read HANDOFF.md and initialize documentation with
`git submodule update --init --recursive`.

Use a focused branch and pull request. Run relevant tests and report automated
and hardware validation separately. Runtime work belongs in C++.

Guides and development logs live in the
[wiki](https://github.com/2thgun/rapid/wiki). Follow its
[documentation workflow](https://github.com/2thgun/rapid/wiki/Documentation-Workflow):
publish wiki changes first, then commit the updated submodule pointer and handoff.

Keep temporary outputs under ignored .local/sessions/. Never commit credentials,
paired keys, recordings, generated binaries or private raw logs to source or wiki.
Preserve rollback copies outside tracked source before deployment. Check the
recorder is idle and verify device state rather than trusting old log entries.
