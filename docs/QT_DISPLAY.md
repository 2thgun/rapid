# Qt touchscreen display

`rapid-qt-display` is the in-progress native replacement for the Chromium kiosk.
It is a separate C++/Qt Quick process: `rapid-pi` continues to own telemetry,
recording, persistence, uploads and the narrow Wi-Fi mode API. The display reads
only the existing local HTTP endpoints, so it cannot affect a recording if it
crashes or is restarted.

## Current scope

- 480 × 320 fullscreen touch layout with Drive, Timing, Vehicle and Tyres pages.
- Local `/api/live` polling at 5 Hz, including stale-telemetry status.
- Existing `/api/v1/network/mode` AP/Home control and `/api/log-status` notice.
- Existing reference steering-wheel PNG as a Qt resource.

The Graphs tab is deliberately marked as incomplete. Chromium remains production
until the Qt display has equivalent graph history, an on-panel touch test and a
full demo rehearsal. Do not replace `rapid-display.service` yet.

## Build

Install Qt 6 development packages that provide Core, Gui, Network, Qml and Quick,
then configure the existing native build with the optional target enabled:

```sh
sudo apt-get install -y qt6-base-dev qt6-declarative-dev
cmake -S cpp -B cpp/build-qt -DCMAKE_BUILD_TYPE=Release \
  -DRAPID_BUILD_LOG_STATUS=ON -DRAPID_BUILD_QT_DISPLAY=ON
cmake --build cpp/build-qt -j2 --target rapid-qt-display
```

For an isolated desktop check, point the display at any running native runtime:

```sh
./cpp/build-qt/rapid-qt-display --endpoint http://127.0.0.1:8000
```

For the GPIO panel, test a direct framebuffer launch only while the current kiosk
is stopped and the recorder is idle. Start with `QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0`
and confirm touch calibration. A later EGLFS test may be faster if the panel's
graphics stack supports it. Do not enable a replacement systemd unit until both
paths have been compared on the physical display.

## Next implementation slice

Port the existing 30-second throttle/brake and G-force graph behavior, including
fresh-sample deduplication and gaps while waiting or paused. Then add an explicit
Qt display service unit that remains disabled by default, plus an on-panel test
and rollback procedure.
