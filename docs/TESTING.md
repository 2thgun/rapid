# Testing

Run Python tests from repository root:

```powershell
python -B -m unittest discover -s tests -v
```

Run C++ tests on Linux in Debug mode. Protocol tests use assertions, so their
Release success is not test evidence. Log-status tests use explicit failures
and remain effective with `NDEBUG` enabled.

Install the native dependencies listed in [operations](OPERATIONS.md) on Linux,
then configure CMake with `-DRAPID_BUILD_LOG_STATUS=ON`. All CTest targets must
pass:

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

The platform-independent log-status checks can also be verified separately in
Release on Linux:

```sh
cmake -S cpp -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target rapid-log-status-tests
ctest --test-dir build/release -R '^rapid-log-status-tests$' --output-on-failure
```

Build the Windows companion using `companion/build-native-daemon.ps1` with
`-Compiler MSVC` or `-Compiler Zig -ZigPath <path>`.
The private `developer/build-companion.ps1` wrapper uses the preserved toolchain
and puts outputs in `developer/build/windows/`. Run its executable with
`--self-test --output-directory <temporary-directory>` without installing it.

The Windows companion self-test validates synthetic LD structure only, not
simulator adapters or live MoTeC interpretation. See [status](STATUS.md) for the
latest completed checks; hosted CI and physical acceptance are tracked separately.

Native runtime tests check malformed input, source pinning, sequence replay,
recorder state, live history, LD structure and scaling, lap extraction, artifact
hashes, abrupt-process recovery and publication retry. Archive tests check Argon2id
authentication, upload offsets, crash-tail truncation, commit recovery and limits.
The network test launches `rapid-pi` on temporary loopback ports and tests UDP,
HTTP, WebSocket history and disconnect finalization. Its temporary recordings
never use the production telemetry directory. Tests retain temporary evidence.
