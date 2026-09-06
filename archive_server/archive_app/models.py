from __future__ import annotations

from datetime import datetime
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


class ArtifactDeclaration(BaseModel):
    model_config = ConfigDict(extra="allow")

    id: str | None = Field(default=None, max_length=128)
    role: str = Field(min_length=1, max_length=64)
    relative_path: str = Field(min_length=1, max_length=512)
    size: int = Field(ge=0)
    sha256: str = Field(pattern=r"^[0-9a-fA-F]{64}$")
    lap_number: int | None = Field(default=None, ge=0, le=100_000)

    @field_validator("role")
    @classmethod
    def clean_role(cls, value: str) -> str:
        return value.strip().lower().replace("-", "_").replace(" ", "_")

    @field_validator("relative_path")
    @classmethod
    def safe_source_path(cls, value: str) -> str:
        value = value.strip()
        if (
            not value
            or "\\" in value
            or value.startswith("/")
            or any(part in {"", ".", ".."} for part in value.split("/"))
            or any(ord(char) < 32 for char in value)
        ):
            raise ValueError("relative_path must be a safe relative POSIX path")
        return value

    @field_validator("sha256")
    @classmethod
    def normalize_hash(cls, value: str) -> str:
        return value.lower()


class SessionManifest(BaseModel):
    model_config = ConfigDict(extra="allow")

    session_id: str = Field(min_length=1, max_length=128)
    simulator: str = Field(min_length=1, max_length=128)
    track: str = Field(min_length=1, max_length=256)
    started_at: datetime | None = None
    ended_at: datetime | None = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    artifacts: list[ArtifactDeclaration] = Field(min_length=1, max_length=10_000)

    @field_validator("session_id", "simulator", "track")
    @classmethod
    def strip_required(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("must not be blank")
        return value

    @model_validator(mode="after")
    def unique_declarations(self) -> "SessionManifest":
        source_ids = [item.id for item in self.artifacts if item.id is not None]
        if len(source_ids) != len(set(source_ids)):
            raise ValueError("artifact ids must be unique")
        source_paths = [item.relative_path.casefold() for item in self.artifacts]
        if len(source_paths) != len(set(source_paths)):
            raise ValueError("artifact relative paths must be unique")
        if self.started_at and self.ended_at and self.ended_at < self.started_at:
            raise ValueError("ended_at must not precede started_at")
        return self
