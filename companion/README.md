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

The pairing credential storage primitive is available in the native daemon. A
paired 256-bit key can be imported into the current user's DPAPI scope with
`--store-auth-key-dpapi PATH` and loaded with `--auth-key-dpapi-file PATH`;
neither operation writes a plaintext key. The Pi now exposes the HTTPS pairing
state API, while the companion HTTPS request/approval UI remains pending.

The planned replacement for private pre-paired bundles is specified in
[the companion pairing contract](PAIRING-CONTRACT.md). It is a development
contract; transport and approval remain an unimplemented customer flow.
