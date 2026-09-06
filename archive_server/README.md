# raPId archive server

This service accepts finalized Pi telemetry bundles over an authenticated,
resumable HTTP API. A bundle is published into `committed/` only after every
declared artifact has its expected SHA-256; incomplete uploads stay in `staging/`
and can resume after either endpoint restarts.

Run it behind TLS (or a private VPN) with secrets mounted as files:

```sh
export RAPID_ARCHIVE_DATA_DIR=/srv/rapid-archive
export RAPID_ARCHIVE_OWNER_PASSWORD_HASH_FILE=/run/secrets/archive_owner_password_hash
export RAPID_ARCHIVE_INGEST_TOKEN_HASH_FILE=/run/secrets/archive_ingest_token_hash
export RAPID_ARCHIVE_HOST=127.0.0.1
export RAPID_ARCHIVE_PORT=8081
export RAPID_ASSETS_DIRECTORY=/path/to/repository/cpp/assets
/path/to/repository/cpp/build/rapid-archive
```

Build the native target using the dependencies in [Pi operations](../docs/OPERATIONS.md).
Existing Argon2id encoded hashes remain supported. Use an Argon2id utility to
generate new hashes; the native service does not depend on Python.
Give the Pi ingest token only the ingest permission; the owner Basic credentials
are required for listing committed sessions. Do not expose the API or any WebDAV
reader without TLS and a reverse-proxy request-size/rate limit.

On the Pi, set `[upload] enabled = true`, its `url`, and its `token`. Race
sessions are queued automatically; the dashboard checkbox enables or disables
upload for the current recording. Upload work occurs off the telemetry path and
is persisted in `upload-queue.db`.

The native ingest service returns relative `/v1/` upload URLs; the native uploader
resolves them against its configured archive URL. The retained Python uploader
is not compatible with these relative responses. Python archive sources remain
available as migration references, not the native launch path.

Resume truncates uncommitted bytes beyond the database offset before writing.
Commit verifies size and SHA-256, publishes atomically on the committed filesystem,
and can recover a rename completed before the database transaction. A committed
artifact cannot be overwritten. Native tests exercise these failure cases.
