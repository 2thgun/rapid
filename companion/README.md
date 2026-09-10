# Windows companion

Build from this directory with:

```powershell
./build-native-daemon.ps1 -Compiler MSVC
```

Setup, packaging and usage are documented in the
[Windows companion wiki](https://github.com/2thgun/rapid/wiki/Windows-Companion).
For offline documentation, initialize the source repository's `wiki/` submodule.

Before building a paired demo bundle, run
`git submodule update --init --recursive` from the repository root. The packager
exports the pinned wiki guides beside the executable. Pairing keys remain private.
