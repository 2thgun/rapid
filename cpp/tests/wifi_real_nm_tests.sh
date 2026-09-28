#!/bin/sh
set -eu

# #50: the "nmcli connection load" + GLib keyfile-escaping path is only
# covered by a fake nmcli in the unit tests. This drives the real rapid-wifi
# helper against real NetworkManager: it writes a Home profile whose SSID
# contains spaces, special characters and a backslash, then checks
# NetworkManager parses the SSID back out unchanged. Activation is exercised
# only when a wifi device is present (a real Pi); on a wifi-less host the
# load/parse round-trip is the part that runs, and the script still exits 0
# so it can run in the local gate.
#
# Usage: wifi_real_nm_tests.sh PATH-TO-rapid-wifi
if [ "$#" -ne 1 ]; then
  echo "usage: wifi_real_nm_tests.sh PATH-TO-rapid-wifi" >&2
  exit 2
fi
helper=$1

if ! command -v nmcli >/dev/null 2>&1; then
  echo "SKIP: nmcli is unavailable; the real-NetworkManager check needs NetworkManager" >&2
  exit 0
fi

root=$(mktemp -d)
# Clean up the connection this check creates, however the script exits.
trap 'rm -rf "$root"; nmcli connection delete rapid-home >/dev/null 2>&1 || true' EXIT

request=$root/wifi-request.json
result=$root/wifi-result.json
# The helper writes to NetworkManager's default system-connections directory;
# that is the only location NM's keyfile backend loads from.
keyfile=/etc/NetworkManager/system-connections/rapid-home.nmconnection

# An SSID with spaces, special characters and a backslash: the GLib keyfile
# escaping must round-trip through real NetworkManager unchanged. printf '%s'
# is used (not echo) so the backslashes reach the file literally regardless of
# the shell's xpg_echo setting.
ssid='Test \ Network & More!'
password='Unique-Real-NM-Passphrase'
printf '%s\n' '{"action":"save","revision":1,"ssid":"Test \\ Network & More!","password":"'"$password"'"}' >"$request"

# Run the real helper against real NetworkManager. On a wifi-less host the
# activation step fails (no wlan0), but the keyfile is written and loaded
# first, which is the part this check verifies.
"$helper" --request-file "$request" --result-file "$result" >/dev/null 2>&1 || true

if [ ! -f "$keyfile" ]; then
  echo "FAIL: the helper did not write the Home keyfile" >&2
  exit 1
fi
mode=$(stat -c '%a' "$keyfile")
if [ "$mode" != "600" ]; then
  echo "FAIL: the installed keyfile is mode $mode, not 600" >&2
  exit 1
fi
# The passphrase must be in the keyfile (NetworkManager needs it) and never
# on an nmcli command line; the helper writes the keyfile directly, so this
# is the only place it appears.
if ! grep -q "psk=$password" "$keyfile"; then
  echo "FAIL: the installed keyfile does not carry the passphrase" >&2
  exit 1
fi

# Load the profile into real NetworkManager and read the SSID back. This is
# the GLib keyfile-escaping round-trip: NetworkManager parses the file the
# helper wrote and must report the exact SSID, backslash and all. The -t
# (terse) form is used because -g escapes a literal backslash in its output.
nmcli connection load "$keyfile" >/dev/null 2>&1
observed=$(nmcli -t -f 802-11-wireless.ssid connection show rapid-home 2>/dev/null | head -n 1 | cut -d: -f2-)
if [ "$observed" != "$ssid" ]; then
  echo "FAIL: NetworkManager parsed the SSID as '$observed', expected '$ssid'" >&2
  exit 1
fi
echo "ok: real NetworkManager loaded the profile and parsed the SSID '$ssid' back unchanged"

# Activation needs a wifi device and a real network; only a physical Pi has
# them. Where one exists, bring the profile up and confirm it activates.
if nmcli -t -f TYPE device status 2>/dev/null | grep -q '^802-11-wireless$'; then
  if nmcli -w 30 connection up rapid-home ifname wlan0 >/dev/null 2>&1; then
    echo "ok: real NetworkManager activated the profile on a wifi device"
  else
    echo "FAIL: real NetworkManager could not activate the profile" >&2
    exit 1
  fi
else
  echo "SKIP: no wifi device here; activation is pending a real Pi"
fi
