# raPId

raPId is a Raspberry Pi racing dashboard, Windows telemetry companion, and MoTeC compatible .ld data recorder.

Engineering preview: the prepared development kit works with existing private
configuration. A fresh-card, terminal-free setup flow is still under development;
no release image is available. See [Get started](https://github.com/2thgun/rapid/wiki/Get-Started)
and the [release checklist](https://github.com/2thgun/rapid/wiki/Release-Acceptance).

## Features

- 480×320 Raspberry Pi touch dashboard with Drive, Timing, Vehicle, Tyres and live Graphs pages.
- Graphs retain 30 seconds of throttle/brake and lateral/longitudinal G-force history while driving; ready and waiting periods render as gaps.
- Windows C++ telemetry companion with adapters for ACC, AC, ACE and iRacing.
- Authenticated binary v4 telemetry with shared-key setup and persistent replay checks.
- Live engineering telemetry in a browser, with selectable traces and history.
- MoTeC compatible session recordings and completed-lap files, available through
  the read-only network share `\\rapid\Telemetry` after session finalization.
- Optional authenticated archive uploads with resumable transfer.
- Journal logging with a small dashboard activity notification.

## Components

| Component | Purpose |
| --- | --- |
| Windows companion | Collect simulator telemetry and send it to the Pi |
| `rapid-pi` | Native C++ dashboard server, telemetry receiver and recorder |
| `rapid-qt-display` | Native Qt touchscreen with five pages and Home/AP/Off control |
| `rapid-log-status` | Native C++ journal activity monitor |
| `rapid-archive` | Native C++ archive ingest service |
| `rapid-setup-server` | Loopback owner enrollment/login and saved desired settings |
| `rapid-firstboot` | Initializes device state; AP onboarding remains incomplete |

## Build

On Linux, install the dependencies in [operations](https://github.com/2thgun/rapid/wiki/Operations), then run
from the repository root:

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON -DRAPID_BUILD_QT_DISPLAY=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

See the [Windows companion guide](https://github.com/2thgun/rapid/wiki/Windows-Companion) for Windows builds and setup.

## Documentation

- [Original Assetto Corsa / Content Manager demo guide](https://github.com/2thgun/rapid/wiki/AC1-Demo-Guide)
- [Architecture](https://github.com/2thgun/rapid/wiki/Architecture)
- [Installation and operations](https://github.com/2thgun/rapid/wiki/Operations)
- [Fresh Pi installation status](image/README.md)
- [Testing](https://github.com/2thgun/rapid/wiki/Testing)
- [Qt touchscreen display migration](https://github.com/2thgun/rapid/wiki/Qt-Display)
- [Telemetry v4 setup and protocol](https://github.com/2thgun/rapid/wiki/Telemetry-v4)
- [Changelog](CHANGELOG.md)

The [wiki](https://github.com/2thgun/rapid/wiki) contains the full guides and
[development log](https://github.com/2thgun/rapid/wiki/Development-Log).
Clone source and its pinned documentation together:

```sh
git clone --recurse-submodules https://github.com/2thgun/rapid.git
```

For an existing checkout, run `git submodule update --init --recursive`.
See [HANDOFF.md](HANDOFF.md) to resume development.
