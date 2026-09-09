# Qt touchscreen display

`rapid-qt-display` is the Qt Quick replacement candidate for the Chromium kiosk.
It is a separate C++ process: `rapid-pi` continues to own telemetry, recording,
persistence, uploads and the narrow Wi-Fi mode API. The display only reads the
local HTTP endpoints, so restarting it cannot affect a recording.

## Current implementation

- 480 by 320 fullscreen touch layout with Drive, Timing, Vehicle, Tyres and
  vertically stacked Graphs pages.
- Local `/api/live` polling at 5 Hz, including stale-telemetry status.
- Existing `/api/v1/network/mode` AP/Home control and `/api/log-status` notice.
- Reference steering-wheel PNG embedded as a Qt resource.
- A rolling 30-second pedal and G-force history that accepts only unique, fresh
  driving samples. Waiting, paused and idle periods are gaps instead of retained
  telemetry drawn as new data.

## Build

Install Qt 6 development packages that provide Core, Gui, Network, Qml and Quick,
then configure the existing native build with the optional target enabled:

```sh
sudo apt-get install -y qt6-base-dev qt6-declarative-dev
cmake -S cpp -B cpp/build-qt -DCMAKE_BUILD_TYPE=Release \
  -DRAPID_BUILD_LOG_STATUS=ON -DRAPID_BUILD_QT_DISPLAY=ON
cmake --build cpp/build-qt -j2 --target rapid-qt-display
```

Qt uses the established Xorg framebuffer session, preserving the existing touch
and tty setup. Chromium remains the production display while Qt's Drive, Timing,
Vehicle and Tyres layouts are brought to feature parity with the browser.

## Promotion criteria

While idle, launch Qt through Xorg and confirm it provides the browser's complete
data layout, adequate touch target sizes and all five pages. The Graphs page must
render its canvas, retain live driving samples and show a gap after returning to
a menu or pausing.

Only then replace `rapid-display.service`; retain its Chromium launcher as an
`.old` backup for rollback.
