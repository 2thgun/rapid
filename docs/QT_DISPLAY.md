# Qt touchscreen display

`rapid-qt-display` is the native Qt Quick replacement for the Chromium kiosk.
It is a separate C++ process: `rapid-pi` continues to own telemetry, recording,
persistence, uploads and the narrow Wi-Fi mode API. The display only reads the
local HTTP endpoints, so restarting it cannot affect a recording.

## Delivered display

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

`rapid-display.service` starts Qt through the established Xorg framebuffer
session. This preserves the existing touch and tty setup. During deployment the
previous Chromium launcher is retained on the Pi as an `.old` backup.

## Verification and rollback

While idle, check `systemctl is-active rapid rapid-display rapid-log-status` and
confirm Drive, Timing, Vehicle, Tyres and Graphs respond to touch. The Graphs
page must retain live driving samples and show a gap after returning to a menu or
pausing.

If the display needs to be restored while idle, replace the Qt launcher in
`rapid-display.service` with its saved Chromium launcher and restart
`rapid-display`.
