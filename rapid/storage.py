from __future__ import annotations

import json
import sqlite3
from pathlib import Path
from .models import LiveState, utcnow

SCHEMA = """
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS drivers (id INTEGER PRIMARY KEY, acc_car_index INTEGER UNIQUE, name TEXT, nationality INTEGER, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, acc_session_index INTEGER, track_id INTEGER, track_name TEXT, car_model TEXT, started_at TEXT NOT NULL, ended_at TEXT, sync_status TEXT NOT NULL DEFAULT 'pending');
CREATE TABLE IF NOT EXISTS laps (id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id), acc_car_index INTEGER, lap_number INTEGER, lap_time_ms INTEGER NOT NULL, delta_ms INTEGER, is_valid INTEGER, completed_at TEXT NOT NULL, sync_status TEXT NOT NULL DEFAULT 'pending', UNIQUE(session_id, acc_car_index, lap_time_ms));
CREATE TABLE IF NOT EXISTS telemetry_packets (id INTEGER PRIMARY KEY, received_at TEXT NOT NULL, packet_type INTEGER NOT NULL, session_id INTEGER REFERENCES sessions(id), car_index INTEGER, normalized_json TEXT NOT NULL, sync_status TEXT NOT NULL DEFAULT 'pending');
"""

class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True); self.connection = sqlite3.connect(path, check_same_thread=False)
        self.connection.row_factory = sqlite3.Row; self.connection.executescript(SCHEMA); self.connection.commit(); self.session_id: int | None = None
    def ensure_session(self, state: LiveState) -> int:
        if self.session_id is None:
            existing = self.connection.execute("SELECT id FROM sessions WHERE acc_session_index IS ? AND ended_at IS NULL ORDER BY id DESC LIMIT 1", (state.session_index,)).fetchone()
            if existing:
                self.session_id = existing["id"]
            else:
                cursor = self.connection.execute("INSERT INTO sessions (acc_session_index, track_name, car_model, started_at) VALUES (?, ?, ?, ?)", (state.session_index, state.track_name, state.car_model, utcnow()))
                self.session_id = cursor.lastrowid
            self.connection.commit()
        return self.session_id
    def write_completed_lap(self, state: LiveState, valid: bool | None = None) -> bool:
        if state.completed_lap_ms is None or state.completed_lap_ms <= 0: return False
        session_id = self.ensure_session(state)
        cursor = self.connection.execute("INSERT OR IGNORE INTO laps (session_id, acc_car_index, lap_number, lap_time_ms, delta_ms, is_valid, completed_at) VALUES (?, ?, ?, ?, ?, ?, ?)", (session_id, state.selected_car_index, state.lap_number, state.completed_lap_ms, state.delta_ms, None if valid is None else int(valid), utcnow()))
        self.connection.commit(); return cursor.rowcount == 1
    def upsert_driver(self, car_index: int, name: str, nationality: int | None) -> None:
        self.connection.execute(
            "INSERT INTO drivers (acc_car_index, name, nationality, created_at) VALUES (?, ?, ?, ?) ON CONFLICT(acc_car_index) DO UPDATE SET name=excluded.name, nationality=excluded.nationality",
            (car_index, name, nationality, utcnow()),
        )
        self.connection.commit()
    def record_packet(self, packet_type: int, data: dict, state: LiveState) -> None:
        """Keep a normalized, timestamped audit stream for later export/sync."""
        self.connection.execute(
            "INSERT INTO telemetry_packets (received_at, packet_type, session_id, car_index, normalized_json) VALUES (?, ?, ?, ?, ?)",
            (utcnow(), packet_type, self.session_id, data.get("car_index"), json.dumps(data, separators=(",", ":"))),
        )
        self.connection.commit()
    def close(self) -> None: self.connection.close()
