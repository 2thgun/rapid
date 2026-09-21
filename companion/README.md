# Windows companion

## Portable single-executable model

The supported way to run the companion is to **run `rapid-telemetry-daemon.exe`
with no arguments and no adjacent files**. Copy the `.exe` anywhere (a flash
drive works) and run it:

1. **First run (unpaired):** the daemon offers pairing. Enter the Pi address and
   the TLS certificate fingerprint shown on the Pi's own display (or its setup
   page) and approve the matching verification code on the Pi. The same
   `--pair` / `run_pairing()` client is used, so the fingerprint check, the
   one-use envelope and the on-device approval are unchanged.
2. The resulting 256-bit key is stored **per Windows user** under
   `%LOCALAPPDATA%\raPId\pairing.key.dpapi`, protected with DPAPI. No plaintext
   key is ever written next to the executable.
3. **Later runs** on the same Windows account just start the tray daemon and
   use the stored credential. A different PC or Windows account pairs once.

No `daemon.conf`, no `telemetry.key`, no installed config path and no command
line are required. `daemon.conf` remains supported as an *optional* advanced
override; `--auth-key`, `--auth-key-file` and `RAPID_TELEMETRY_KEY` remain
available for migrated kits and automation.

### Offline / demo recording

The same executable also serves the offline case as a deliberate opt-in mode:
`--no-forward` (or `no_forward=true` in the optional config) records the local
`.ld` fallback with no Pi, no pairing and no key. It never pairs. Normal
forwarded operation needs pairing; offline recording is the only mode that
intentionally runs without a credential.

### Automation and console-less hosts

`--headless` runs without the tray/console UI and never shows a blocking error
dialog. A run with no attached console (scheduled task, automation harness)
fails fast with a stderr line and a non-zero exit code instead of waiting on a
`MessageBox` — see #42.

## Build

Build from this directory with:

```powershell
./build-native-daemon.ps1 -Compiler MSVC
```

Setup, packaging and usage are documented in the
[Windows companion wiki](https://github.com/2thgun/rapid/wiki/Windows-Companion).
For offline documentation, initialize the source repository's `wiki/` submodule.

Before building a paired demo bundle, run
`git submodule update --init --recursive` from the repository root. The packager
exports the pinned wiki guides beside the executable. Pairing keys remain private.

The native `--self-test` also exercises Windows CNG Curve25519 key agreement
and public-key export, covering the cryptographic material required by the
pairing envelope. On the pinned toolchain the exported public blob is validated
as the expected 72-byte CNG representation and its 32-byte wire segment is
checked before future HTTPS transport.

The same self-test now exercises the Windows CNG HKDF-SHA-256 and AES-256-GCM
operations used by the envelope contract, including authenticated-data
verification and a decrypt round trip. This validates the platform crypto
layer before the HTTPS request client is connected.
The self-test also checks independent RFC 5869 HKDF and NIST AES-256-GCM
known-answer vectors.
The code derivation used for the two-screen comparison is also checked against
a fixed Pi-compatible vector, including its field delimiters and byte order.

The native daemon can pair directly with the Pi over its pinned HTTPS endpoint.
It creates the X25519 request, displays the comparison code, unwraps the
one-use envelope and stores the resulting 256-bit key in the current user's
DPAPI scope. Use `--pairing-url`, `--pairing-label` and
`--certificate-fingerprint`; `--store-auth-key-dpapi` and
`--auth-key-dpapi-file` remain available for migrated installations.
The Pi-side HTTPS integration test exercises a real X25519 request and decrypts
the one-use envelope end to end.
That integration also pairs two independent PCs and verifies revocation of one
does not remove the other.
Credential writes are committed through a flushed temporary file and atomic
replace so an interrupted settings recovery cannot leave a truncated blob.
The reader clears the protected blob after unprotecting it, including when
validation or DPAPI unprotect fails.
The writer rejects anything other than the contract's 32-byte telemetry key.
The native self-test also checks that invalid key widths are rejected before
DPAPI is called.
It also rejects a malformed persisted DPAPI record before attempting
unprotect, covering interrupted or corrupted settings recovery.

The companion can verify a Pi setup certificate before a pairing client uses it:

```powershell
./rapid-telemetry-daemon.exe --verify-setup-url https://192.168.1.64:8002/setup `
  --certificate-fingerprint <64 lowercase hex characters>
```

This explicitly pinned probe is also used by the pairing client. It fails on a
non-HTTPS URL or fingerprint mismatch and does not write credentials.

The full flow and its remaining hardware acceptance requirements are specified
in [the companion pairing contract](PAIRING-CONTRACT.md). Pairing belongs to
the two main telemetry programs; first-time provisioning remains separate.

## Legacy launchers and the MSI

`START-RAPID.cmd` and `start-rapid-daemon.vbs` are **legacy compatibility**
launchers kept only for existing demo kits that still ship `daemon.conf` /
`telemetry.key`; the portable path above does not use them. They prefer
`%LOCALAPPDATA%\raPId\pairing.key.dpapi` when it exists; the legacy bundle key
remains a compatibility fallback for migrated kits.

The MSI installs the same executable and adds Start-menu shortcuts: one to run
the companion directly (which pairs on first use) and one to `PAIR.cmd`, so the
manual `--pair` entry point is discoverable without a terminal (#49).
