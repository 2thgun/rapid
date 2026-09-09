# Pi operations

Connect with `ssh rapid@rapid` and use the hostname rather than a DHCP address.

```sh
systemctl status rapid rapid-display rapid-log-status
journalctl -u rapid -u rapid-display -u rapid-log-status -f
curl http://127.0.0.1:8000/api/live
curl http://127.0.0.1:8001/api/log-status
```

The dashboard owns `/dev/tty1`. Its output is journal-only; do not restore
`journal+console`. Finalized recordings are read from `\\rapid\Telemetry`.
Keep the current GPIO overlay and touch calibration unless a physical-panel test
proves a replacement works.

The dashboard services intentionally do not wait for `network-online.target` at
boot. The dashboard only depends on local HTTP; telemetry clients reconnect when
the LAN is ready.

## First-boot provisioning

Use cloud-init only to provision a new Pi. A provisioning payload that builds the
runtime or restarts display services must not remain active for ordinary boots:
it leaves the panel at a login console while it runs. After a successful first
boot, preserve the seed as a private `.old` backup, replace it with an inert
cloud config, and create `/etc/cloud/cloud-init.disabled`. Keep the installed
services and network configuration; do not erase recordings or paired keys.

The Qt kiosk uses Xorg directly on the GPIO framebuffer. See [Qt setup](QT_DISPLAY.md) for display dependencies, installation and rollback.

## Native runtime deployment

Install the distribution build dependencies, then build and test:

```sh
sudo apt-get install -y cmake g++ pkg-config libsystemd-dev libsqlite3-dev \
  libcurl4-openssl-dev libssl-dev nlohmann-json3-dev libboost-system-dev \
  libtomlplusplus-dev libargon2-dev
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON
cmake --build cpp/build -j2
ctest --test-dir cpp/build --output-on-failure
```

`rapid-pi` serves the dashboard, telemetry APIs, WebSocket history and recorder.
It reads the existing TOML configuration and `RAPID_*` environment overrides.
The default asset directory is `cpp/assets/` relative to the service working
directory; override with `RAPID_ASSETS_DIRECTORY` if installing elsewhere.
The native service respects `[acc] enabled` and `[upload] enabled`.
For companion authentication, follow [v4 migration](TELEMETRY_V4.md). A configured
`[app] companion_key` enables v4-only reception; an empty key keeps legacy reception.

Install `systemd/rapid.service` only after the native checks pass and the current
recorder is idle. Its executable is `cpp/build/rapid-pi`. Preserve the previous
unit in the deployment backup for rollback.

Native recording publishes session bundles when driving ends or the sender
disconnects. Successfully published channel spools remain as `.old` recovery
copies; failed publication leaves its spool available for retry. These recovery
copies consume disk space and currently require deliberate operator retention.

Install `systemd/rapid-log-status.service` into `/etc/systemd/system/` and enable it.
It runs as `rapid`, with journal access. Install `systemd/journald-rapid.conf` as
`/etc/systemd/journald.conf.d/rapid.conf` on the dedicated Pi to enable bounded
persistent logs and disable console forwarding.

## Dashboard Wi-Fi mode control

`rapid-network-mode.service` lets the dashboard select Home Wi-Fi, the
`rapid-demo` access point, or Wi-Fi Off. It uses a root-owned NetworkManager
worker; `rapid-pi` can only write a queued `home`, `ap` or `off` request in
`/run/rapid-network`.

Install the service and script together, then enable it:

```sh
sudo install -m 0755 systemd/rapid-network-mode /usr/local/sbin/rapid-network-mode
sudo install -m 0644 systemd/rapid-network-mode.service /etc/systemd/system/rapid-network-mode.service
sudo systemctl daemon-reload
sudo systemctl enable --now rapid-network-mode
```

The dashboard cycles `WIFI HOME`, `WIFI AP` and `WIFI OFF`. The requesting
browser disconnects while `wlan0` changes networks. In AP mode the Pi is
`192.168.1.64`; in Home mode use its hostname or DHCP address. Selecting Off
disconnects Wi-Fi until Home or AP is selected from the physical panel.

Set the intended boot/home profile explicitly on a deployed Pi instead of relying
on NetworkManager's connection-list order:

```sh
sudo sh -c 'printf "%s\\n" "RAPID_HOME_CONNECTION=your-saved-wifi-profile" > /etc/rapid-network-mode.conf'
sudo nmcli connection modify rapid-demo connection.autoconnect yes connection.autoconnect-priority -100
sudo nmcli connection modify your-saved-wifi-profile connection.autoconnect yes connection.autoconnect-priority 100
sudo systemctl restart rapid-network-mode
```

This makes the saved home connection the normal boot path. The dashboard button
still switches to the AP explicitly, and the AP remains a fallback when Home is
unavailable. Keep `/etc/rapid-network-mode.conf` private because its profile name
can identify a local network.

Before deployment, preserve changed files and units in a unique `.old` backup and
confirm the recorder is idle. After deployment check all three services, both APIs,
the share and the panel. To roll back, restore that backup and reload systemd;
disable the native log unit if restoring the older dashboard implementation.
