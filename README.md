# raPId

raPId is a Raspberry Pi racing dashboard, Windows telemetry companion, and MoTeC compatible .ld data recorder.

**Status: engineering preview.** No release has been published yet. 0.9.9 is
the first planned release.

## Features

- A dashboard for a 480×320 touchscreen with five pages: Drive, Timing, Vehicle,
  Tyres and Graphs.
- ACC, Assetto Corsa, Assetto Corsa EVO and iRacing, all through one lightweight Windows
  program (`rapid-telemetry-daemon.exe`) that needs no installation.
- MoTeC i2 compatible recordings: the whole session plus one file per lap. All
  simulators share the same channel layout.
- Setup from a browser, with no peripherals needed: the Pi starts its own Wi-Fi
  network, and you claim it, connect it to home Wi-Fi, set the orientation and
  calibrate touch from a phone.
- Pairing: each PC gets its own key after you approve a code on the Pi.
  Telemetry is authenticated and protected against replay.
- Everything runs locally. No internet or account is needed.

## Documentation

The [wiki](https://github.com/2thgun/rapid/wiki) is the manual:

- [Getting started](https://github.com/2thgun/rapid/wiki/Getting-Started)
- [First-time setup](https://github.com/2thgun/rapid/wiki/First-Time-Setup)
- [Windows companion](https://github.com/2thgun/rapid/wiki/Windows-Companion)
- [Driving and recording](https://github.com/2thgun/rapid/wiki/Driving-and-Recording)
- [Troubleshooting](https://github.com/2thgun/rapid/wiki/Troubleshooting)
- [Architecture](https://github.com/2thgun/rapid/wiki/Architecture),
  [telemetry protocol](https://github.com/2thgun/rapid/wiki/Telemetry-Protocol),
  [building](https://github.com/2thgun/rapid/wiki/Building),
  [testing](https://github.com/2thgun/rapid/wiki/Testing)

User-visible changes are listed in [CHANGELOG.md](CHANGELOG.md).

## Quick build

The Pi components build on Linux. The dependencies are listed in
[Building](https://github.com/2thgun/rapid/wiki/Building).

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON -DRAPID_BUILD_QT_DISPLAY=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The Windows companion builds on Windows (PowerShell):

```powershell
./companion/build-native-daemon.ps1 -Compiler MSVC
```

## Repository layout

| Path | Contents |
| --- | --- |
| `cpp/` | Pi programs: runtime, pairing, setup, Qt display, log monitor, archive |
| `companion/` | Windows companion and its build scripts |
| `installer/` | MSI definition |
| `packaging/` | Debian package, systemd units, default configuration |
| `image/` | Raspberry Pi image profile |
| `wiki/` | The wiki, as a Git submodule (`git submodule update --init`) |

Contributing: see [CONTRIBUTING](.github/CONTRIBUTING.md).

## License

MIT. See [LICENSE](LICENSE).
