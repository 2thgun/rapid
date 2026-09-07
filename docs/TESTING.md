# Testing

Run C++ tests on Linux in Debug and Release modes. Protocol and log-status tests
use explicit failures and remain effective with `NDEBUG` enabled. Hosted CI runs
the full native suite in both configurations.

Install the native dependencies listed in [operations](OPERATIONS.md) on Linux,
then configure CMake with `-DRAPID_BUILD_LOG_STATUS=ON`. All CTest targets must
pass:

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DRAPID_BUILD_LOG_STATUS=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Run the complete suite again in Release on Linux:

```sh
cmake -S cpp -B build/release -DCMAKE_BUILD_TYPE=Release -DRAPID_BUILD_LOG_STATUS=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Build the Windows companion using `companion/build-native-daemon.ps1` with
`-Compiler MSVC` or `-Compiler Zig -ZigPath <path>`.
Run its executable with
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
