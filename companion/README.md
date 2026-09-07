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
tree. Binaries are excluded from Git.

## Current source versus older installed binaries

The current source defaults to `rapid:9001`, 50 Hz and authenticated binary v4.
Configure the same key on both ends using the [v4 guide](../docs/TELEMETRY_V4.md).
Use `--protocol v3` for an older Pi receiver configured without a companion key.
Local recording is currently unconditional despite the parsed opt-in flag.

Older saved binaries and the compatibility wrapper use broadcast/10 Hz defaults.
The preserved private runtime must not be mistaken for a build of today's source.

Without a supported simulator process, the native daemon only checks the process
table every five seconds. It does not open shared memory, send heartbeats, or
record.

## Launch and installation

For venue requirements and one-off use, see [portable setup](../docs/PORTABLE_SETUP.md).

For an explicit one-off current-source run on a trusted LAN:

```powershell
./rapid-telemetry-daemon.exe --protocol v3 --pi-host rapid --sample-rate 50
```

The installer registers an absolute launcher path for the current Windows user.
Do not run it as a test. Before using the installer with a fresh build, persist
`protocol=v3` for a keyless Pi, or `protocol=v4` and an absolute `auth_key_file`
path, in `%LOCALAPPDATA%/raPId/daemon.conf`; the VBS launcher passes no arguments.

Offline LD checks do not validate simulator units, game detection or MoTeC i2
lap interpretation. Drive multiple laps and inspect one log per simulator.
