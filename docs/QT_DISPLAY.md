# Qt touchscreen display

The C++ Qt Quick display reads the local runtime API. It provides the same
channels as the browser dashboard across Drive, Timing, Vehicle, Tyres and
Graphs. The recorder remains a separate process.

Page buttons are 48 pixels high. The bordered Wi-Fi button opens a menu with
62-pixel Home, Access Point and Wi-Fi Off targets. Home is selected by the network
worker at startup.

The two graphs are stacked vertically. They show a rolling 30-second window,
deduplicate source samples and leave gaps for menus, stale data, HTTP failures
and missing channels. Empty history shows the grid without inventing readings.

## Build and test

Install native dependencies from [operations](OPERATIONS.md), then:

```sh
sudo apt-get install -y qt6-base-dev qt6-declarative-dev \
  qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript
cmake -S cpp -B cpp/build-qt -DCMAKE_BUILD_TYPE=Release \
  -DRAPID_BUILD_QT_DISPLAY=ON -DRAPID_BUILD_LOG_STATUS=ON
cmake --build cpp/build-qt -j2
ctest --test-dir cpp/build-qt --output-on-failure
```

Qt uses the established Xorg framebuffer and touch calibration. The kiosk sets
`QT_QUICK_BACKEND=software` for the GPIO framebuffer.

## Install and recover

While the recorder is idle, preserve the current display service as a private
`.old` backup. Install the launcher and service:

```sh
chmod +x systemd/rapid-qt-kiosk
sudo install -m 0644 systemd/rapid-display.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl restart rapid-display
```

Check all five pages, the Wi-Fi selector, the service journal and
`http://127.0.0.1:8000/healthz`. The browser dashboard remains available at
`http://rapid:8000/`. Restore the saved service and reload systemd to roll back.

## Isolated graph fixture

`cpp/build-qt/rapid-qt-tests --serve` exposes synthetic readings on loopback port
18080 for two minutes. Launch Qt with `--endpoint http://127.0.0.1:18080` to inspect
changing graph lines and periodic menu gaps. This fixture never sends UDP,
changes Wi-Fi or writes recordings. Production uses port 8000.
