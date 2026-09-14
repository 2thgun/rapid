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
pairing envelope.

The pairing credential storage primitive is available in the native daemon. A
paired 256-bit key can be imported into the current user's DPAPI scope with
`--store-auth-key-dpapi PATH` and loaded with `--auth-key-dpapi-file PATH`;
neither operation writes a plaintext key. The Pi now exposes the HTTPS pairing
state API, and the physical panel can approve a displayed request through its
private local handoff. The companion HTTPS request, envelope decryption and
reconnect client remain pending.

The companion can verify a Pi setup certificate before a pairing client uses it:

```powershell
./rapid-telemetry-daemon.exe --verify-setup-url https://192.168.1.64:8002/setup `
  --certificate-fingerprint <64 lowercase hex characters>
```

This explicitly pinned probe is the only path that accepts the Pi's self-signed
certificate. It fails on a non-HTTPS URL or fingerprint mismatch and does not
write credentials. The full request, code comparison, envelope decryption and
DPAPI handoff are still pending.

The planned replacement for private pre-paired bundles is specified in
[the companion pairing contract](PAIRING-CONTRACT.md). The Pi transport and
physical panel approval are implemented; the Windows request, envelope and
reconnect client remain the unimplemented customer flow.
