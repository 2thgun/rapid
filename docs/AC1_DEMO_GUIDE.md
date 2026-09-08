# Original Assetto Corsa demo: setup, rehearsal and recovery

This guide is for the original **Assetto Corsa (AC1)** on Windows, including
sessions launched through **Content Manager**, with the paired raPId Pi. Use the
prepared native companion bundle and the matching tested Pi release.

Treat the demo as ready only after the rehearsal below passes on the PC, car,
track and network you will actually use. Automated checks cover packet handling,
recording and dashboard behavior; they do not establish that a physical wheel,
Content Manager session or MoTeC installation has been tested successfully.

## 1. What to bring and arrange beforehand

- The configured Pi, its attached display, tested power supply and power cable.
  A Pi with only its power connector available needs its Wi-Fi arranged before
  arrival; a network cable alone cannot connect hardware without Ethernet.
- The complete prepared companion folder, including
  `rapid-telemetry-daemon.exe`, `START-RAPID.cmd`, `daemon.conf`, `telemetry.key`
  and this guide. Keep a second copy on another drive. The key is private and
  must match the Pi; an executable by itself is insufficient.
- A Windows PC with original AC installed and able to drive the chosen car and
  track. Content Manager, the game's normal prerequisites, any selected content,
  and the wheel/pedal drivers must already work on that PC.
- Permission to run the companion, write recordings, use the local network and
  retrieve recordings. Rehearse any Windows application approval prompts before
  the presentation; do not disable the venue's security controls.
- A known working local network that lets the PC reach the Pi. Prefer your own
  already tested router if the venue cannot guarantee this. Guest Wi-Fi and some
  hotspots isolate clients. Internet is unnecessary for raPId's local operation;
  separately confirm that AC/Steam and the selected content launch offline if
  that is the intended setup.
- The Pi's current IP address and the credentials used for its SSH/file share,
  stored privately. Preconfigure a reachable Wi-Fi network while still at home
  if the Pi has no keyboard. An unreachable Pi cannot be repaired over SSH.
- MoTeC i2 already installed and tested if opening a recording is part of the
  presentation. Keep a successfully opened rehearsal recording as a fallback.

No compiler or Python installation is needed to run the native companion.
Run the actual packaged executable on the target Windows PC before the demo to
catch missing runtime DLLs or blocked executables.

Content Manager is an alternative AC launcher. raPId attaches to the running
simulation, so merely opening Content Manager does not start telemetry.
See the [Content Manager project page](https://assettocorsa.club/content-manager.html).
raPId reads AC's native shared memory; it does not require enabling an AC Python
app, a Content Manager plugin or ACC's broadcasting configuration. Custom Shaders
Patch is not a raPId prerequisite. Rehearse the exact mod/CSP combination if the
chosen demo content requires it.

## 2. Set up the Pi and Windows companion

1. Power the Pi from its tested supply and wait for the dashboard to appear.
   Confirm that touch works and the steering-wheel image is visible on Drive.
   Resolve a persistent `PWR LIMIT` warning before driving; use the tested supply
   and cable. Boot time depends on the Pi and network, so allow setup time.
2. Join the Windows PC to the Pi's network. From a Windows browser open
   `http://rapid:8000/healthz`, then `http://rapid:8000/`. If `rapid` does not
   resolve, use the Pi's current IPv4 address in both addresses.
3. Extract/copy the **whole companion folder** to a writable local directory,
   such as `C:\Users\YourName\Documents\raPId Demo`. Do not run inside a ZIP or
   from a read-only drive.
4. Open the adjacent `daemon.conf` in a text editor. The prepared values are:

   ```ini
   pi_host=rapid
   pi_port=9001
   sample_rate=50
   protocol=v4
   auth_key_file=telemetry.key
   output_directory=recordings
   local_recording=true
   no_forward=false
   ```

   If necessary, replace only `pi_host=rapid` with the Pi's current IPv4 address.
   The host value has no `http://` or `:8000`. Keep the file as plain UTF-8 text
   without a BOM. Do not add quotes around values. Keep the existing paired key.
5. If another raPId companion is running, use its tray menu **Exit** before
   starting this bundle. There must be one companion instance. A second launch
   exits quietly; it does **not** apply the new folder's configuration.
6. Double-click `START-RAPID.cmd`. It starts the native companion with the
   adjacent configuration and key. Look for its tray icon, including the hidden
   icons area. Right-click it and choose **Show status**.

The launcher sets its working directory to the bundle folder. Logs are in
`recordings\rapid-daemon-native.log`; the local recording fallback also saves there.
The optional `start-rapid-daemon.vbs` launcher requires Windows Script Host;
`START-RAPID.cmd` is the normal entry point.

For a manual launch, open PowerShell in the extracted folder and run:

```powershell
.\rapid-telemetry-daemon.exe --config .\daemon.conf --auth-key-file .\telemetry.key
```

Always pass this bundle's configuration explicitly when troubleshooting; an
older `%LOCALAPPDATA%\raPId\daemon.conf` can otherwise be loaded. The private
`telemetry.key` file must contain the same 64 hexadecimal characters as the Pi's
key, without quotes or a BOM. Pairing instructions are in
[Telemetry v4](TELEMETRY_V4.md). Do not change one end's key on demo day or use a
test-fixture key. This paired setup uses v4.

## 3. Start original AC through Content Manager

1. Use the same Windows login and desktop session for Content Manager, AC and
   the companion. Start them normally, without elevation, unless the venue's
   already tested setup specifically requires otherwise. AC exposes `Local\`
   shared-memory objects, which belong to the Windows session; a companion
   running as a different user's service cannot read this session's telemetry.
   [Windows namespace behavior](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces).
2. Close other supported simulators. For the first rehearsal select an installed
   stock car and track, a solo Practice session and normal driving. Replays,
   showrooms and a Content Manager menu do not substitute for driving.
3. Launch the session from Content Manager, finish loading and enter the car.
   The real AC process is `acs.exe` or `acs_x86.exe`. The companion detects
   these processes; it does not treat `Content Manager.exe` or the original
   launcher's menu as a running simulation.
4. Allow about five seconds for process discovery, then leave the pause/menu
   state and drive. On the tray status, samples should advance. On the Pi,
   expect `AC connected`, `REC` on Timing, changing RPM and responding pedals.
5. While safely stationary, press and release each pedal, turn the wheel left
   and right, then center it. Compare the dashboard with the physical controls
   and the game's indicators. Select first gear and check `1`; neutral is `N`
   and reverse is `R`.

If the status is unclear, run this in PowerShell:

```powershell
Get-Process acs,acs_x86,rapid-telemetry-daemon -ErrorAction SilentlyContinue |
    Select-Object ProcessName,Id,SessionId

$PiHost = 'rapid' # Replace with the Pi's current IPv4 address if needed.
Invoke-RestMethod "http://${PiHost}:8000/api/live" |
    Select-Object simulator,schema_version,companion_connected,companion_daemon_state,
        samples_received,telemetry_fresh,telemetry_age_ms,recording,recorded_samples,
        packets_auth_failed,packets_invalid,packets_replayed,packets_lost
```

During real driving, `simulator` must be `AC`, `schema_version` must be `4`,
`telemetry_fresh` and `recording` must be true, and the sample counters must
advance when you rerun the command. Authentication/invalid/replay failures
should not increase. A successful HTTP health check or the companion's forwarded
packet count alone does not prove that the Pi accepted UDP telemetry.

## 4. Rehearse the exact presentation

Complete this once before departure and repeat the short control check after
connecting at the venue. Allow enough time to complete two clean laps.

| Check | Pass condition |
| --- | --- |
| Cold start | Pi shows the dashboard without a keyboard, console overlay or keyring prompt; all five touch tabs work. |
| AC launch through Content Manager | `acs.exe`/`acs_x86.exe` starts; Pi identifies `AC`, accepts v4 and records changing telemetry. |
| Drive page | Pedals rise and return to zero, RPM/speed/gears agree with the game, and the wheel turns in the expected direction. |
| Graphs page | Pedal and G-force graphs are stacked vertically; throttle/brake actions produce distinct traces over the last 30 seconds. |
| Timing and metadata | Car/track/driver identify the loaded session; current lap advances, crossing the line advances lap number and supplies the completed lap time. |
| Vehicle and Tyres | Available fuel, tyre and vehicle readings change plausibly; distinguish unsupported values from an actual telemetry failure. |
| Pause/menu and resume | Stopped samples become stale and graphs show a gap; unpausing/driving resumes new samples rather than extending a frozen trace. |
| Session end | Return to the menu/exit the driving session; Pi becomes `IDLE`, a new completed bundle appears, and the PC local fallback is saved. |
| MoTeC export | Copy the completed bundle and open `full-session.ld` in the target MoTeC installation; check pedals, steering and lap markers against the drive. |
| Second launch | Start another AC session from Content Manager and confirm fresh telemetry and a separate finalized recording. |

Keep a short video or screenshots of the successful rehearsal and its recording.
Do not mark a failed or unperformed row as passed. A different car, track, PC,
network or mod configuration needs its own short rehearsal.

For the presentation: show Drive while operating the controls, show the pedal
and G-force traces on Graphs, complete a lap, then end the driving session and
open the finalized recording. Keep the setup that passed rehearsal unchanged.

## 5. Finish and collect the recording

1. End the driving session normally and return to Content Manager. A pause can
   split a recording; use a deliberate session end when presenting export.
   Wait for Timing to show `IDLE` and for the recording files to appear.
2. Open `\\rapid\Telemetry` in Windows File Explorer, or
   `\\<Pi IPv4 address>\Telemetry` if hostname lookup fails. Use the configured
   share credentials when requested. The share is read-only.
3. Copy the latest finalized bundle to the PC. It contains `full-session.ld`,
   `full-session.ldx`, `manifest.json` and any completed-lap files under `laps\`.
   Keep the `.ld` and `.ldx` together. The Pi's `/api/live` response exposes
   `last_bundle_path` to identify the bundle after it has finalized.
4. Open `full-session.ld` in MoTeC i2 and check the recorded controls and lap
   markers. Completed-lap files become available when the session finalizes;
   they are not published immediately while driving continues.
5. Use the companion tray's **Open telemetry folder** to collect its local `.ld`
   fallback and log. Exit the companion through its tray menu before removing
   the bundle/key from a borrowed PC. Preserve the recordings you need first.
6. Shut down the Pi cleanly from the PC when finished:

   ```powershell
   ssh rapid@rapid
   ```

   Then, in the Pi shell, run `sudo shutdown -h now`. Wait for shutdown to finish
   before removing power. Do not unplug the Pi while it is recording or publishing.

## 6. Symptoms that can stop the demo

| Symptom | Likely cause and recovery |
| --- | --- |
| Pi cannot be reached by SSH or browser | Check power and the network it joined. Try its current IPv4 address. Confirm both devices are on the tested network with client isolation disabled by its owner. Restore the preconfigured network if the keyboardless Pi joined the wrong one. |
| `rapid` fails but the IP works | Hostname discovery/DNS is unavailable. Set `pi_host` to that IPv4 address, exit the companion, then relaunch. Use the same address in the browser/share. |
| Browser works, telemetry does not | HTTP uses TCP 8000; telemetry uses PC-to-Pi UDP 9001. Check the host/port, matching key, companion status and counters. Ask the network owner to allow that traffic; do not turn off the firewall. |
| Forwarded count grows but Pi counters do not | UDP send success is not delivery confirmation. Check the destination IP, UDP policy and Pi authentication counters. Close companions on other PCs; the Pi accepts one active source, and a configured `companion_host` may still pin an old PC. |
| Companion disappears on launch | It normally lives in the tray. Check hidden icons and Task Manager. If an older instance owns the tray, exit that instance first; a duplicate launch exits quietly. |
| Missing DLL, blocked application or launch error | Run the prepared executable on this PC during rehearsal. Use the tested package and venue-approved application procedure. A source checkout is not a portable executable package. |
| Key/configuration error | Confirm the adjacent files exist and the launcher is from the same folder. Use a BOM-free key/config, valid `key=value` lines and the paired key. Do not paste a password or a quoted string into the key file. |
| Dormant while Content Manager is open | Launch an actual driving session. Confirm `acs.exe` or `acs_x86.exe` in Task Manager and wait for the five-second discovery interval. |
| AC process detected, still waiting for telemetry | Finish loading/enter the car; avoid showroom/replay. Run AC and companion in the same Windows session with compatible permissions. Close other sims and relaunch the AC session. |
| Works before travel, not after Wi-Fi/IP change | Update `pi_host` to the verified current IPv4 address, then exit/relaunch the companion after the network is ready. |
| Wrong simulator or competing readings | Close every other supported simulator and companion instance, then relaunch this companion and AC. Do not run two rigs against the paired Pi during the demo. |
| `Waiting for fresh telemetry`, stationary graph or gaps | Unpause and drive; verify AC is producing samples. Inspect the PC log and Pi counters. Sustained loss during real driving needs network/power investigation. A gap during a pause is expected. |
| Wrong pedal/steering behavior | Check the controls in AC first, then compare the dashboard on the stock rehearsal car. Do not present a physically reversed, stuck or mis-scaled control as correct. |
| Steering-wheel image missing | Pi assets or server binary do not match the prepared release. Restore/deploy the complete tested release while recording is idle, then restart the display. |
| No new `.ld` while still driving | Normal publication timing: end the driving session and wait for `IDLE`. If still absent, check free disk space and the Pi journal; preserve recordings and recovery spools. |
| Share is unavailable but dashboard works | SMB uses TCP 445 and separate share credentials. Try the Pi IP and configured credentials. Do not enable insecure guest access as a workaround. Use the PC fallback recording or retrieve the Pi bundle over the already tested SSH route. |
| Save failed or output is empty | Check `recordings\rapid-daemon-native.log`, available disk space and write permission. Run from a writable local folder and end the session cleanly. Force-killing the companion can lose its in-memory fallback recording. |
| Pi screen stuck but browser works | The display service needs attention. While idle, SSH in and restart `rapid-display`; check its journal. Keep the browser dashboard available as the temporary presentation screen. |
| Pi service restart leaves a blank screen | After restarting `rapid`, explicitly start `rapid-display` as shown below; the display stops with the runtime. |

Pi diagnostics, run after `ssh rapid@rapid`:

```sh
systemctl is-active rapid rapid-display rapid-log-status
curl -fsS http://127.0.0.1:8000/api/live
journalctl -u rapid -u rapid-display -u rapid-log-status -n 80 --no-pager
df -h /home/rapid/raPId
```

For a display-only fault, run `sudo systemctl restart rapid-display`. If the
runtime itself needs a restart, first ensure recording is idle, then run:

```sh
sudo systemctl restart rapid
sudo systemctl start rapid-display
systemctl is-active rapid rapid-display rapid-log-status
```

Keep configuration, paired keys, the state database and recordings intact.
Restore a previous tested release only from its retained `.old` backup, while
idle, using the [operations guide](OPERATIONS.md).

## 7. Fallbacks and limits to explain honestly

If the Pi panel fails but telemetry reaches the Pi, present the same live
dashboard at `http://<Pi address>:8000/` in the PC browser. The engineering view
is at `/telemetry`. These still require a working Pi runtime and network.

If the network cannot be recovered, the prepared companion has local recording
enabled. Drive a short session, end it normally, and present its saved `.ld`
from `recordings\` in MoTeC. This demonstrates local recording; it does not establish
that Pi forwarding is working. If the game itself will not launch, use the
previous rehearsal recording/video and identify it as recorded footage.

Some dashboard fields are not supplied by the AC v4 channel set: best-lap and
sector splits, for example, may remain blank. A last-lap time needs a completed
lap. The dashboard graphs are a 30-second browser history that resets on reload;
they are not the full 50 Hz recording. Pi recordings do not reconstruct exact
elapsed time through lost UDP packets. Live control calibration and MoTeC lap
interpretation still need the rehearsal checks above.

If a rehearsal failure remains unresolved, keep that feature out of the live
presentation and use the explicitly identified fallback. Record the symptom and
save the PC log/Pi journal so the issue can be investigated without losing progress.
