# Windows companion

`rapid-telemetry-daemon.exe` reads ACC, Assetto Corsa, Assetto Corsa EVO and
iRacing telemetry and sends it to a paired Pi as authenticated v4 packets. It
is one portable executable. On first run it pairs with the Pi and stores its
key for the current Windows user with DPAPI. It needs no config file, key file
or launcher.

How to use it: [Windows companion](https://github.com/2thgun/rapid/wiki/Windows-Companion).
How to build it: [Building](https://github.com/2thgun/rapid/wiki/Building#windows-companion).

## Build and test

```powershell
./build-native-daemon.ps1 -Compiler MSVC          # or -Compiler Zig -ZigPath <zig.exe>
./rapid-telemetry-daemon.exe --self-test --output-directory "$env:TEMP\rapid-test"
./build-msi.ps1 -ExecutablePath ./rapid-telemetry-daemon.exe -OutputPath ../dist/raPId-Companion.msi
```

The self-test covers the simulator adapters against their official
shared-memory layouts, LD output, the v4 encoder, the Windows CNG crypto
(X25519, HKDF-SHA256, AES-256-GCM, with known-answer vectors), DPAPI storage
and the pairing client. It also writes the v4 and pairing fixtures that the
Linux tests consume.

## Files

| File | Purpose |
| --- | --- |
| `native/rapid-telemetry-daemon.cpp` | The companion: adapters, v4 sender, tray, pairing client, local LD writer, self-test. |
| `native/adapter_selftest_fixtures.hpp` | Shared-memory fixtures for the adapter self-tests. |
| `build-native-daemon.ps1` | Builds the executable with MSVC or Zig and stamps the build version. |
| `build-msi.ps1` | Builds the MSI from `../installer/raPIdCompanion.wxs` (WiX 4). |
| `PAIR.cmd` | Runs `--pair`. The MSI's Start-menu shortcut uses it. |
| `PAIRING-CONTRACT.md` | The pairing protocol contract. |
| `daemon.conf.example` | Example of the optional settings file. |
| `START-RAPID.cmd`, `start-rapid-daemon.vbs`, `install-demo.cmd`, `install-rapid-daemon.ps1`, `package-demo.ps1`, `build-demo-installer.ps1` | Legacy demo-kit tooling, used by kits that carry a plain `telemetry.key`. The portable executable doesn't need any of it. |
