from __future__ import annotations

import sqlite3
from pathlib import Path


SCHEMA = """
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
"""


def connect(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path, timeout=30, isolation_level=None)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    connection.execute("PRAGMA busy_timeout = 30000")
    return connection


def initialize(path: Path) -> None:
    with connect(path) as connection:
        connection.execute("PRAGMA journal_mode = WAL")
        connection.execute("PRAGMA synchronous = FULL")
        connection.executescript(SCHEMA)


def check(path: Path) -> bool:
    try:
        with connect(path) as connection:
            return connection.execute("PRAGMA quick_check").fetchone()[0] == "ok"
    except sqlite3.Error:
        return False
