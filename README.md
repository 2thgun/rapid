# raPId

raPId is a Raspberry Pi racing dashboard, Windows telemetry companion, and MoTeC compatible .ld data recorder.

## Features

- 480×320 Raspberry Pi touch dashboard with Drive, Timing, Vehicle and Tyres pages.
- Windows C++ telemetry companion with adapters for ACC, AC, ACE and iRacing.
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
| `rapid-log-status` | Native C++ journal activity monitor |
| `rapid-archive` | Native C++ archive ingest service |

## Build

On Linux, install the dependencies in [operations](docs/OPERATIONS.md), then run
from the repository root:

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

See the [Windows companion guide](companion/README.md) for Windows builds and setup.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Installation and operations](docs/OPERATIONS.md)
- [Testing](docs/TESTING.md)
- [Changelog](CHANGELOG.md)
