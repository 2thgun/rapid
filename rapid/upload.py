"""Durable, best-effort upload of finalized telemetry bundles."""
from __future__ import annotations

import json
import logging
import sqlite3
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from .motec import FinalizedBundle

LOG = logging.getLogger(__name__)


class ArchiveUploader:
    def __init__(self, url: str, token: str, queue_path: Path, state):
        self.url, self.token, self.queue_path, self.state = url.rstrip("/"), token, queue_path, state
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def enabled(self) -> bool:
        return bool(self.url and self.token)

    def start(self) -> None:
        if not self.enabled:
            return
        self.queue_path.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(self.queue_path) as db:
            db.execute("CREATE TABLE IF NOT EXISTS jobs (path TEXT PRIMARY KEY, state TEXT NOT NULL, error TEXT, updated_at REAL NOT NULL)")
        self._thread = threading.Thread(target=self._run, name="rapid-archive-upload", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set(); self._wake.set()
        if self._thread: self._thread.join(timeout=3)

    def enqueue(self, bundle: FinalizedBundle) -> None:
        if not self.enabled or not bundle.manifest.get("metadata", {}).get("upload_after_session"):
            return
        with sqlite3.connect(self.queue_path) as db:
            db.execute("INSERT INTO jobs(path,state,error,updated_at) VALUES(?, 'pending', NULL, ?) ON CONFLICT(path) DO UPDATE SET state='pending', error=NULL, updated_at=excluded.updated_at", (str(bundle.directory), time.time()))
        self.state.update(upload_state="queued")
        self._wake.set()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                with sqlite3.connect(self.queue_path) as db:
                    row = db.execute("SELECT path FROM jobs WHERE state != 'complete' ORDER BY updated_at LIMIT 1").fetchone()
                if row:
                    self.state.update(upload_state="uploading")
                    self._upload(Path(row[0]))
                    with sqlite3.connect(self.queue_path) as db: db.execute("UPDATE jobs SET state='complete', error=NULL, updated_at=? WHERE path=?", (time.time(), row[0]))
                    self.state.update(upload_state="complete")
                    continue
            except Exception as exc:
                LOG.warning("archive upload deferred: %s", exc)
                if 'row' in locals() and row:
                    with sqlite3.connect(self.queue_path) as db: db.execute("UPDATE jobs SET state='retry', error=?, updated_at=? WHERE path=?", (str(exc)[:500], time.time(), row[0]))
                self.state.update(upload_state="retrying")
            self._wake.wait(30); self._wake.clear()

    def _request(self, method: str, url: str, data: bytes | None = None, headers: dict[str, str] | None = None):
        request = urllib.request.Request(url, data=data, method=method, headers={"Authorization": f"Bearer {self.token}", **(headers or {})})
        return urllib.request.urlopen(request, timeout=30)

    def _upload(self, directory: Path) -> None:
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        with self._request("POST", f"{self.url}/v1/sessions", json.dumps(manifest, separators=(",", ":")).encode(), {"Content-Type": "application/json"}) as response:
            declared = json.load(response)
        for item, artifact in zip(manifest["artifacts"], declared["artifacts"], strict=True):
            source = directory / item["relative_path"]
            with self._request("HEAD", artifact["url"]) as response:
                offset = int(response.headers.get("Upload-Offset", "0"))
            with source.open("rb") as handle:
                handle.seek(offset)
                while chunk := handle.read(1024 * 1024):
                    with self._request("PATCH", artifact["url"], chunk, {"Upload-Offset": str(offset), "Content-Type": "application/offset+octet-stream"}) as response:
                        offset = int(response.headers["Upload-Offset"])
        self._request("POST", declared["commit_url"]).close()
