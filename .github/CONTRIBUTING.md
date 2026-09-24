# Contributing to raPId

1. Clone with the wiki: `git clone --recurse-submodules https://github.com/2thgun/rapid.git`.
2. Read [PROJECT.md](../PROJECT.md) for the working method, and
   [Building](https://github.com/2thgun/rapid/wiki/Building) and
   [Testing](https://github.com/2thgun/rapid/wiki/Testing) for the commands.
3. Work on a focused branch and open a pull request. CI must be green.
4. In the pull request, report automated results and hardware results
   separately. Say which device-facing behavior was **not** exercised on a real
   Pi.
5. If behavior changed, update the wiki page that describes it and
   `CHANGELOG.md`. Push the wiki first, then commit the submodule pointer.

Never commit credentials, keys, recordings, generated binaries or raw logs.
