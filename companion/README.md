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

Without a supported simulator process, the native daemon only checks the process
table every five seconds. It does not open shared memory, send heartbeats, or
record.

## Launch and installation

For venue requirements and one-off use, see [portable setup](../docs/PORTABLE_SETUP.md).
For original Assetto Corsa and Content Manager, use the complete
[demo guide](../docs/AC1_DEMO_GUIDE.md). Prepare a paired portable folder from an
already built native executable:

```powershell
./package-demo.ps1 -ExecutablePath ./rapid-telemetry-daemon.exe -AuthKeyPath C:/private/telemetry.key -Destination C:/Demos/raPId
```

The destination's parent must exist outside the repository. The package contains
the private paired key, local recording folder and `START-RAPID.cmd`; copy the
whole folder to the demo PC. Existing packages are preserved as `.old` folders.

For the paired AC1 demo package, the setup EXE copies the bundle into
`%LOCALAPPDATA%\raPId\AC1 Demo` and opens that folder. Double-click
`START-RAPID.cmd` there after connecting the PC to the Pi. It does not install
anything into AC, Content Manager, CSP, wheel drivers, or Windows login startup.

To distribute a single setup executable, build it from a prepared private demo
folder on Windows:

```powershell
./build-demo-installer.ps1 -BundlePath C:/Demos/raPId -OutputPath C:/Demos/raPId-AC1-Demo-Setup.exe
```

The resulting EXE contains the private paired key, so keep it out of Git and
share it only with the intended demo PC.

For a one-off run with a paired Pi:

```powershell
./rapid-telemetry-daemon.exe --protocol v4 --pi-host rapid --sample-rate 50 --auth-key-file ./telemetry.key
```

The installer registers an absolute launcher path for the current Windows user.
Do not run it as a test. Before using the installer with a fresh build, persist
`protocol=v3` for a keyless Pi, or `protocol=v4` and an absolute `auth_key_file`
path, in `%LOCALAPPDATA%/raPId/daemon.conf`. The native VBS launcher uses adjacent
`daemon.conf` and `telemetry.key` when present; otherwise the normal per-user
configuration applies.

Offline LD checks do not validate simulator units, game detection or MoTeC i2
lap interpretation. Drive multiple laps and inspect one log per simulator.
