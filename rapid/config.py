from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    acc_enabled: bool = False
    acc_host: str = "192.168.1.89"
    acc_port: int = 9000
    acc_local_port: int = 9000
    acc_password: str = ""
    display_name: str = "raPId"
    protocol_version: int = 4
    update_interval_ms: int = 100
    app_host: str = "0.0.0.0"
    app_port: int = 8000
    database_path: Path = Path("data/rapid.db")
    companion_port: int = 9001
    companion_host: str = ""
    companion_key: str = ""
    telemetry_directory: Path = Path("data/telemetry")
    upload_enabled: bool = False
    upload_url: str = ""
    upload_token: str = ""
    upload_policy: str = "races"
    upload_queue_path: Path = Path("data/upload-queue.db")
    upload_retention_days: int = 14


def load_settings(path: str | Path | None = None) -> Settings:
    """Load local TOML settings, then allow environment overrides for service use."""
    source = Path(path or os.environ.get("RAPID_CONFIG", "config.toml"))
    raw: dict = {}
    if source.exists():
        with source.open("rb") as handle:
            raw = tomllib.load(handle)
    acc, app = raw.get("acc", {}), raw.get("app", {})
    upload = raw.get("upload", {})

    def value(env: str, table: dict, key: str, default):
        return os.environ.get(env, table.get(key, default))

    def boolean(env: str, table: dict, key: str, default: bool) -> bool:
        raw_value = value(env, table, key, default)
        if isinstance(raw_value, bool):
            return raw_value
        normalized = str(raw_value).strip().lower()
        if normalized in {"1", "true", "yes", "on"}:
            return True
        if normalized in {"0", "false", "no", "off"}:
            return False
        raise ValueError(f"{env} must be a boolean value")

    upload_policy = str(value("RAPID_UPLOAD_POLICY", upload, "policy", "races")).lower()
    if upload_policy not in {"races", "all", "manual"}:
        raise ValueError("RAPID_UPLOAD_POLICY must be races, all, or manual")
    return Settings(
        acc_enabled=boolean("RAPID_ACC_ENABLED", acc, "enabled", False),
        acc_host=str(value("RAPID_ACC_HOST", acc, "host", Settings.acc_host)),
        acc_port=int(value("RAPID_ACC_PORT", acc, "port", Settings.acc_port)),
        acc_local_port=int(value("RAPID_ACC_LOCAL_PORT", acc, "local_port", Settings.acc_local_port)),
        acc_password=str(value("RAPID_ACC_PASSWORD", acc, "password", "")),
        display_name=str(value("RAPID_DISPLAY_NAME", acc, "display_name", "raPId")),
        protocol_version=int(value("RAPID_PROTOCOL_VERSION", acc, "protocol_version", 4)),
        update_interval_ms=int(value("RAPID_UPDATE_INTERVAL_MS", acc, "update_interval_ms", 100)),
        app_host=str(value("RAPID_APP_HOST", app, "host", "0.0.0.0")),
        app_port=int(value("RAPID_APP_PORT", app, "port", 8000)),
        database_path=Path(str(value("RAPID_DATABASE_PATH", app, "database_path", "data/rapid.db"))),
        companion_port=int(value("RAPID_COMPANION_PORT", app, "companion_port", 9001)),
        companion_host=str(value("RAPID_COMPANION_HOST", app, "companion_host", "")),
        companion_key=str(value("RAPID_COMPANION_KEY", app, "companion_key", "")),
        telemetry_directory=Path(str(value(
            "RAPID_TELEMETRY_DIRECTORY", app, "telemetry_directory", "data/telemetry"
        ))),
        upload_enabled=boolean("RAPID_UPLOAD_ENABLED", upload, "enabled", False),
        upload_url=str(value("RAPID_UPLOAD_URL", upload, "url", "")).rstrip("/"),
        upload_token=str(value("RAPID_UPLOAD_TOKEN", upload, "token", "")),
        upload_policy=upload_policy,
        upload_queue_path=Path(str(value(
            "RAPID_UPLOAD_QUEUE_PATH", upload, "queue_path", "data/upload-queue.db"
        ))),
        upload_retention_days=max(0, int(value(
            "RAPID_UPLOAD_RETENTION_DAYS", upload, "retention_days", 14
        ))),
    )
