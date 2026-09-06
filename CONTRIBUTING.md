# Contributing to raPId

Use a focused branch and pull request. Do not commit credentials, recordings,
generated binaries, screenshots, caches, or Pi-specific configuration.

Run the relevant tests and report both automated and hardware validation. Changes
to deployment, recording, transport, or the Windows installer must include a
rollback note.

New application runtime work belongs in `cpp/`. Check the current deployment in
`docs/STATUS.md`; a native source implementation does not establish deployment.
Switch `rapid.service` only after native tests, a rollback backup and device checks.
