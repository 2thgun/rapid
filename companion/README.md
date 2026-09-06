# Windows telemetry companion

Native C++20/Win32 telemetry collection for ACC, AC, ACE, and iRacing. The daemon
waits in the tray, detects simulator processes, forwards normalized telemetry and
writes 48-channel MoTeC logs. Live acceptance in all four simulators is outstanding.

## Build and verify

```powershell
./build-native-daemon.ps1 -Compiler MSVC
./rapid-telemetry-daemon.exe --self-test --output-directory "$env:TEMP/rapid-test"
```

Alternatively use `-Compiler Zig -ZigPath C:/tools/zig/zig.exe`.
`-OutputDirectory` and `-CacheDirectory` keep build artifacts outside the source
tree. The private workspace has `developer/build-companion.ps1` configured for
the preserved portable toolchain. Binaries are excluded from Git.

## Current source versus older installed binaries

The current source defaults to `rapid:9001`, 50 Hz and a v4 selector that requires
a key. However, its active sender still emits v3 JSON; the v4 encoder is not wired
in. Use `--protocol v3` for the current trusted-LAN Pi receiver. Do not describe
this connection as authenticated. Local recording is currently unconditional,
despite the newly parsed opt-in flag. These integration gaps are in the backlog.

Older saved binaries and the compatibility wrapper use broadcast/10 Hz defaults.
The preserved private runtime must not be mistaken for a build of today's source.

Without a supported simulator process, the native daemon only checks the process
table every five seconds. It does not open shared memory, send heartbeats, or
record. The PowerShell fallback has different idle behavior and is legacy only.

## Launch and installation

For an explicit one-off current-source run on a trusted LAN:

```powershell
./rapid-telemetry-daemon.exe --protocol v3 --pi-host rapid --sample-rate 50
```

The installer registers an absolute launcher path for the current Windows user.
Do not run it as a test. Before using the installer with a fresh build, persist
`protocol=v3` in `%LOCALAPPDATA%/raPId/daemon.conf`; the VBS launcher passes no
arguments. The legacy wrapper cannot pass the new protocol/authentication flags.

Offline LD checks do not validate simulator units, game detection or MoTeC i2
lap interpretation. Drive multiple laps and inspect one log per simulator.
