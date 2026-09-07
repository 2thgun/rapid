# Taking raPId to another simulator

Bring the configured Pi, its display, suitable power supply, network cables as
needed, and a tested Windows companion executable. A fresh Pi requires the
installation described in [operations](OPERATIONS.md).

## Venue requirements

- A Windows PC running a supported simulator: ACC, AC, ACE or iRacing. Live
  acceptance across all four remains outstanding; this is not universal simulator support.
- Permission to run the companion and access simulator telemetry. No compiler
  or Python installation is needed to run the native companion. Any runtime DLL
  requirements depend on how the executable was built; test the actual binary
  on a clean PC before relying on an executable-only package.
- PC and Pi on a reachable local network. Configure the Pi for the venue Wi-Fi
  or connect it by Ethernet. Guest/client-isolated networks may prevent communication.
- Network policy allowing PC-to-Pi UDP port 9001. Browser diagnostics use TCP
  port 8000; optional recording access over SMB uses TCP port 445.
- A writable companion output directory. The current companion also records
  locally on the PC, so allow disk space and agree where venue recordings belong.

## Start a session

Run from PowerShell in the companion directory:

```powershell
.\rapid-telemetry-daemon.exe --protocol v3 --pi-host rapid --sample-rate 50
```

Use the Pi's current address with `--pi-host` if hostname discovery does not work.
The explicit v3 flag is required for this documented setup: the current source
defaults to a v4 selector whose sender integration is unfinished. v3 is intended
for a trusted local network and does not authenticate telemetry.

Open `http://rapid:8000/healthz` from the PC to check HTTP reachability. That does
not prove UDP is allowed; start driving and confirm the dashboard receives data.
The companion stays dormant while no supported simulator process is running.

Internet and an archive server are not required for local dashboard/recording
use. MoTeC is needed only to inspect exported recordings. No startup installation
is needed for a one-off companion run; quit it afterward.
