from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _secret(name: str, default_file: str | None = None) -> str:
    value = os.environ.get(name)
    if value:
        return value.strip()
    file_name = os.environ.get(f"{name}_FILE", default_file)
    if file_name:
        try:
            return Path(file_name).read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            pass
    raise RuntimeError(f"{name} or {name}_FILE must be configured")


def _positive_int(name: str, default: int) -> int:
    try:
        value = int(os.environ.get(name, str(default)))
    except ValueError as exc:
        raise RuntimeError(f"{name} must be an integer") from exc
    if value <= 0:
        raise RuntimeError(f"{name} must be positive")
    return value


@dataclass(frozen=True)
class Settings:
    data_dir: Path
    database_path: Path
    staging_dir: Path
    committed_dir: Path
    owner_username: str
    owner_password_hash: str
    ingest_token_hash: str
    max_artifact_bytes: int = 16 * 1024 * 1024 * 1024

    @classmethod
    def from_env(cls) -> "Settings":
        data_dir = Path(os.environ.get("RAPID_ARCHIVE_DATA_DIR", "/data"))
        return cls(
            data_dir=data_dir,
            database_path=Path(
                os.environ.get("RAPID_ARCHIVE_DATABASE_PATH", str(data_dir / "archive.sqlite3"))
            ),
            staging_dir=Path(
                os.environ.get("RAPID_ARCHIVE_STAGING_DIR", str(data_dir / "staging"))
            ),
            committed_dir=Path(
                os.environ.get("RAPID_ARCHIVE_COMMITTED_DIR", str(data_dir / "committed"))
            ),
            owner_username=os.environ.get("RAPID_ARCHIVE_OWNER_USERNAME", "owner"),
            owner_password_hash=_secret(
                "RAPID_ARCHIVE_OWNER_PASSWORD_HASH",
                "/run/secrets/archive_owner_password_hash",
            ),
            ingest_token_hash=_secret(
                "RAPID_ARCHIVE_INGEST_TOKEN_HASH",
                "/run/secrets/archive_ingest_token_hash",
            ),
            max_artifact_bytes=_positive_int(
                "RAPID_ARCHIVE_MAX_ARTIFACT_BYTES", 16 * 1024 * 1024 * 1024
            ),
        )

    def prepare(self) -> None:
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self.staging_dir.mkdir(parents=True, exist_ok=True)
        self.committed_dir.mkdir(parents=True, exist_ok=True)


@dataclass(frozen=True)
class DavSettings:
    committed_dir: Path
    username: str
    password_hash: str
    host: str = "0.0.0.0"
    port: int = 8080

    @classmethod
    def from_env(cls) -> "DavSettings":
        return cls(
            committed_dir=Path(
                os.environ.get("RAPID_ARCHIVE_COMMITTED_DIR", "/data/committed")
            ),
            username=os.environ.get("RAPID_ARCHIVE_DAV_USERNAME", "archive-reader"),
            password_hash=_secret(
                "RAPID_ARCHIVE_DAV_PASSWORD_HASH",
                "/run/secrets/archive_dav_password_hash",
            ),
            host=os.environ.get("RAPID_ARCHIVE_DAV_HOST", "0.0.0.0"),
            port=_positive_int("RAPID_ARCHIVE_DAV_PORT", 8080),
        )
