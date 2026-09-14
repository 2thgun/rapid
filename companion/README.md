# Windows companion

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

The shipped `START-RAPID.cmd` and `start-rapid-daemon.vbs` launchers prefer
`%LOCALAPPDATA%\raPId\pairing.key.dpapi` when it exists. This lets a recovered
per-user pairing start without copying or exposing `telemetry.key`; the legacy
bundle key remains a compatibility fallback for existing demo kits.
