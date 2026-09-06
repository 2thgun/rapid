"""Resumable, authenticated ingest API for finalized raPId bundles."""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import uuid
from datetime import datetime, timezone
from pathlib import Path

from fastapi import Depends, FastAPI, HTTPException, Request, Response, status

from .auth import require_ingest, require_owner
from .config import Settings
from .db import connect, initialize
from .models import SessionManifest
from .storage import generated_artifact_path, inside, publish_session, remove_staging_session, sha256_file, slug


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _manifest_hash(manifest: SessionManifest) -> str:
    payload = json.dumps(manifest.model_dump(mode="json"), sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(payload).hexdigest()


def _session_row(connection: sqlite3.Connection, session_id: str) -> sqlite3.Row:
    row = connection.execute("SELECT * FROM sessions WHERE id = ?", (session_id,)).fetchone()
    if row is None:
        raise HTTPException(404, "unknown session")
    return row


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    settings.prepare()
    initialize(settings.database_path)
    app = FastAPI(title="raPId archive", version="0.1.0")
    app.state.settings = settings

    @app.get("/healthz")
    def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.post("/v1/sessions", dependencies=[Depends(require_ingest)], status_code=status.HTTP_201_CREATED)
    def declare(manifest: SessionManifest, request: Request) -> dict:
        digest = _manifest_hash(manifest)
        with connect(settings.database_path) as connection:
            previous = connection.execute("SELECT * FROM sessions WHERE source_session_id = ?", (manifest.session_id,)).fetchone()
            if previous:
                if previous["manifest_sha256"] != digest:
                    raise HTTPException(409, "session_id was already declared with a different manifest")
                session_id = previous["id"]
            else:
                session_id = uuid.uuid4().hex
                date = (manifest.started_at or datetime.now(timezone.utc)).strftime("%Y-%m-%d")
                archive_relpath = f"{date}/{slug(manifest.simulator)}/{slug(manifest.track)}/{session_id}"
                connection.execute("BEGIN IMMEDIATE")
                connection.execute(
                    "INSERT INTO sessions VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'staging', ?, NULL)",
                    (session_id, manifest.session_id, digest, json.dumps(manifest.model_dump(mode="json"), sort_keys=True),
                     manifest.simulator, manifest.track, manifest.started_at.isoformat() if manifest.started_at else None,
                     archive_relpath, _now()),
                )
                for ordinal, item in enumerate(manifest.artifacts, 1):
                    artifact_id = uuid.uuid4().hex
                    archive_path = generated_artifact_path(item.role, item.relative_path, item.lap_number, ordinal)
                    media_type = "application/octet-stream"
                    staging_name = f"{artifact_id}.part"
                    artifact_status = "uploaded" if item.size == 0 else "declared"
                    connection.execute(
                        "INSERT INTO artifacts (id, session_id, source_artifact_id, role, source_relative_path, archive_relpath, expected_size, expected_sha256, media_type, staging_filename, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        (artifact_id, session_id, item.id, item.role, item.relative_path, archive_path,
                         item.size, item.sha256, media_type, staging_name, artifact_status),
                    )
                connection.commit()
        with connect(settings.database_path) as connection:
            artifacts = connection.execute("SELECT id, expected_size, upload_offset, status FROM artifacts WHERE session_id = ? ORDER BY rowid", (session_id,)).fetchall()
        base = str(request.base_url).rstrip("/")
        return {"session_id": session_id, "commit_url": f"{base}/v1/sessions/{session_id}/commit", "artifacts": [
            {"id": row["id"], "size": row["expected_size"], "offset": row["upload_offset"], "status": row["status"], "url": f"{base}/v1/artifacts/{row['id']}"} for row in artifacts
        ]}

    @app.head("/v1/artifacts/{artifact_id}", dependencies=[Depends(require_ingest)])
    def artifact_offset(artifact_id: str) -> Response:
        with connect(settings.database_path) as connection:
            row = connection.execute("SELECT expected_size, upload_offset FROM artifacts WHERE id = ?", (artifact_id,)).fetchone()
        if not row:
            raise HTTPException(404, "unknown artifact")
        return Response(headers={"Upload-Offset": str(row["upload_offset"]), "Upload-Length": str(row["expected_size"])})

    @app.patch("/v1/artifacts/{artifact_id}", dependencies=[Depends(require_ingest)])
    async def append_artifact(artifact_id: str, request: Request) -> Response:
        try:
            offset = int(request.headers["upload-offset"])
        except (KeyError, ValueError):
            raise HTTPException(400, "Upload-Offset header is required")
        body = await request.body()
        with connect(settings.database_path) as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute("SELECT * FROM artifacts WHERE id = ?", (artifact_id,)).fetchone()
            if not row:
                raise HTTPException(404, "unknown artifact")
            if row["upload_offset"] != offset:
                connection.rollback()
                return Response(status_code=409, headers={"Upload-Offset": str(row["upload_offset"])})
            end = offset + len(body)
            if end > row["expected_size"]:
                raise HTTPException(413, "artifact exceeds declared size")
            path = inside(settings.staging_dir, f"{row['session_id']}/{row['staging_filename']}")
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("ab") as handle:
                handle.write(body); handle.flush(); os.fsync(handle.fileno())
            artifact_status = "uploaded" if end == row["expected_size"] else "uploading"
            connection.execute("UPDATE artifacts SET upload_started=1, upload_offset=?, status=? WHERE id=?", (end, artifact_status, artifact_id))
            connection.commit()
        return Response(status_code=204, headers={"Upload-Offset": str(end)})

    @app.post("/v1/sessions/{session_id}/commit", dependencies=[Depends(require_ingest)])
    def commit(session_id: str) -> dict:
        with connect(settings.database_path) as connection:
            connection.execute("BEGIN IMMEDIATE")
            session = _session_row(connection, session_id)
            if session["status"] == "committed":
                return {"session_id": session_id, "status": "committed", "path": session["archive_relpath"]}
            artifacts = connection.execute("SELECT * FROM artifacts WHERE session_id=? ORDER BY rowid", (session_id,)).fetchall()
            for artifact in artifacts:
                source = inside(settings.staging_dir, f"{session_id}/{artifact['staging_filename']}")
                if artifact["expected_size"] == 0 and not source.exists():
                    source.parent.mkdir(parents=True, exist_ok=True)
                    source.touch()
                if artifact["status"] != "uploaded" or not source.is_file() or sha256_file(source) != artifact["expected_sha256"]:
                    raise HTTPException(409, "all declared artifacts must be fully uploaded and verified")
            connection.execute("UPDATE sessions SET status='committing' WHERE id=?", (session_id,)); connection.commit()
        try:
            manifest_path = inside(settings.staging_dir, f"{session_id}/manifest.json")
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest_path.write_text(session["manifest_json"] + "\n", encoding="utf-8")
            published = publish_session(settings.staging_dir, settings.committed_dir, session_id, session["archive_relpath"],
                [(manifest_path, "manifest.json"), *[(inside(settings.staging_dir, f"{session_id}/{a['staging_filename']}"), a["archive_relpath"]) for a in artifacts]])
            with connect(settings.database_path) as connection:
                connection.execute("BEGIN IMMEDIATE"); connection.execute("UPDATE sessions SET status='committed', committed_at=? WHERE id=?", (_now(), session_id)); connection.execute("UPDATE artifacts SET status='committed' WHERE session_id=?", (session_id,)); connection.commit()
            remove_staging_session(settings.staging_dir, session_id)
            return {"session_id": session_id, "status": "committed", "path": str(published.relative_to(settings.committed_dir))}
        except Exception:
            with connect(settings.database_path) as connection:
                connection.execute("UPDATE sessions SET status='staging' WHERE id=? AND status='committing'", (session_id,))
            raise

    @app.get("/v1/sessions", dependencies=[Depends(require_owner)])
    def list_sessions() -> list[dict]:
        with connect(settings.database_path) as connection:
            rows = connection.execute("SELECT id, source_session_id, simulator, track, started_at, committed_at, archive_relpath FROM sessions WHERE status='committed' ORDER BY committed_at DESC").fetchall()
        return [dict(row) for row in rows]

    return app
