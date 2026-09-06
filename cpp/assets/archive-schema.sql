
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    source_session_id TEXT NOT NULL UNIQUE,
    manifest_sha256 TEXT NOT NULL,
    manifest_json TEXT NOT NULL,
    simulator TEXT NOT NULL,
    track TEXT NOT NULL,
    started_at TEXT,
    archive_relpath TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK(status IN ('staging', 'committing', 'committed')),
    created_at TEXT NOT NULL,
    committed_at TEXT
);

CREATE TABLE IF NOT EXISTS artifacts (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    source_artifact_id TEXT,
    role TEXT NOT NULL,
    source_relative_path TEXT NOT NULL,
    archive_relpath TEXT NOT NULL,
    expected_size INTEGER NOT NULL CHECK(expected_size >= 0),
    expected_sha256 TEXT NOT NULL,
    media_type TEXT NOT NULL,
    upload_started INTEGER NOT NULL DEFAULT 0,
    upload_offset INTEGER NOT NULL DEFAULT 0,
    staging_filename TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('declared', 'uploading', 'uploaded', 'committed')),
    UNIQUE(session_id, archive_relpath),
    UNIQUE(session_id, source_artifact_id)
);

CREATE INDEX IF NOT EXISTS idx_sessions_committed
ON sessions(status, committed_at DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_artifacts_session
ON artifacts(session_id, id);
