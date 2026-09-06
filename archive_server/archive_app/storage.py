from __future__ import annotations

import hashlib
import os
import re
import shutil
import unicodedata
from pathlib import Path, PurePosixPath


_SAFE_NAME = re.compile(r"[^a-z0-9]+")
_SAFE_FILE = re.compile(r"[^A-Za-z0-9._-]+")


def slug(value: str, fallback: str = "unknown", limit: int = 64) -> str:
    ascii_value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    result = _SAFE_NAME.sub("-", ascii_value.lower()).strip("-")
    return (result or fallback)[:limit].rstrip("-")


def safe_filename(value: str, fallback: str = "artifact.bin") -> str:
    name = _SAFE_FILE.sub("-", Path(value).name).strip(".-")
    return (name or fallback)[:160]


def generated_artifact_path(
    role: str, relative_path: str, lap_number: int | None, ordinal: int
) -> str:
    suffix = Path(relative_path).suffix.lower()
    role = role.lower().replace("-", "_")
    if role in {"manifest", "session_manifest"}:
        return "manifest.json"
    if role in {"full", "full_session", "session", "session_log", "full_log"}:
        return "full-session.ld" if suffix != ".ldx" else "full-session.ldx"
    if role in {"index", "ldx", "session_index", "full_session_index"}:
        return "full-session.ldx"
    if role in {"lap", "lap_log", "completed_lap"}:
        lap = lap_number if lap_number is not None else ordinal
        return f"laps/lap-{lap:03d}.ld"
    return f"artifacts/{ordinal:03d}-{safe_filename(relative_path)}"


def inside(root: Path, relative_path: str) -> Path:
    root = root.resolve()
    candidate = (root / Path(*PurePosixPath(relative_path).parts)).resolve()
    if candidate == root or root not in candidate.parents:
        raise ValueError("path escapes configured storage root")
    return candidate


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def fsync_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as reader, destination.open("xb") as writer:
        shutil.copyfileobj(reader, writer, 1024 * 1024)
        writer.flush()
        os.fsync(writer.fileno())


def publish_session(
    staging_root: Path,
    committed_root: Path,
    session_id: str,
    archive_relpath: str,
    artifacts: list[tuple[Path, str]],
) -> Path:
    final_dir = inside(committed_root, archive_relpath)
    if final_dir.exists():
        return final_dir

    publish_dir = inside(staging_root, f"{session_id}.publishing")
    if publish_dir.exists():
        shutil.rmtree(publish_dir)
    publish_dir.mkdir(parents=True)
    try:
        for source, archive_path in artifacts:
            fsync_copy(source, inside(publish_dir, archive_path))
        final_dir.parent.mkdir(parents=True, exist_ok=True)
        os.replace(publish_dir, final_dir)
    except Exception:
        if publish_dir.exists():
            shutil.rmtree(publish_dir)
        raise
    return final_dir


def remove_staging_session(staging_root: Path, session_id: str) -> None:
    path = inside(staging_root, session_id)
    if path.exists():
        shutil.rmtree(path)
