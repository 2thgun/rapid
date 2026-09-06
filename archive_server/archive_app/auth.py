from __future__ import annotations

import hmac
from typing import Annotated

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError, VerifyMismatchError
from fastapi import Depends, HTTPException, Request, status
from fastapi.security import HTTPBasic, HTTPBasicCredentials, HTTPBearer, HTTPAuthorizationCredentials

from .config import Settings


_hasher = PasswordHasher()
_basic = HTTPBasic(auto_error=False)
_bearer = HTTPBearer(auto_error=False)


def verify_argon2(secret_hash: str, candidate: str) -> bool:
    try:
        return bool(_hasher.verify(secret_hash, candidate))
    except (InvalidHashError, VerificationError, VerifyMismatchError):
        return False


def get_settings(request: Request) -> Settings:
    return request.app.state.settings


def require_owner(
    credentials: Annotated[HTTPBasicCredentials | None, Depends(_basic)],
    settings: Annotated[Settings, Depends(get_settings)],
) -> str:
    valid_username = bool(credentials) and hmac.compare_digest(
        credentials.username.encode("utf-8"), settings.owner_username.encode("utf-8")
    )
    valid_password = bool(credentials) and verify_argon2(
        settings.owner_password_hash, credentials.password
    )
    if not (valid_username and valid_password):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid owner credentials",
            headers={"WWW-Authenticate": 'Basic realm="raPId archive"'},
        )
    return settings.owner_username


def require_ingest(
    credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(_bearer)],
    settings: Annotated[Settings, Depends(get_settings)],
) -> None:
    if (
        credentials is None
        or credentials.scheme.lower() != "bearer"
        or not verify_argon2(settings.ingest_token_hash, credentials.credentials)
    ):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid ingest token",
            headers={"WWW-Authenticate": "Bearer"},
        )
