from __future__ import annotations

from dataclasses import dataclass, field, fields
from datetime import datetime, timezone
import threading
from typing import Any


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat()


@dataclass
class LiveState:
    connected: bool = False
    acc_connected: bool = False
    simulator: str | None = None
    connection_id: int | None = None
    selected_car_index: int | None = None
    gear: int | None = None
    rpm: int | None = None
    steering_angle: float | None = None
    g_x: float | None = None
    g_y: float | None = None
    g_z: float | None = None
    throttle: float | None = None
    brake: float | None = None
    companion_connected: bool = False
    companion_daemon_state: str | None = None
    companion_source_host: str | None = None
    companion_received_at: str | None = None
    speed_kmh: float | None = None
    fuel: float | None = None
    tc: float | None = None
    abs_activity: float | None = None
    pit_limiter: bool | None = None
    lap_position: float | None = None
    wheel_speed_fl: float | None = None
    wheel_speed_fr: float | None = None
    wheel_speed_rl: float | None = None
    wheel_speed_rr: float | None = None
    pressure_fl: float | None = None
    pressure_fr: float | None = None
    pressure_rl: float | None = None
    pressure_rr: float | None = None
    core_temp_fl: float | None = None
    core_temp_fr: float | None = None
    core_temp_rl: float | None = None
    core_temp_rr: float | None = None
    suspension_fl: float | None = None
    suspension_fr: float | None = None
    suspension_rl: float | None = None
    suspension_rr: float | None = None
    damage_front: float | None = None
    damage_rear: float | None = None
    damage_left: float | None = None
    damage_right: float | None = None
    damage_center: float | None = None
    current_lap_ms: int | None = None
    completed_lap_ms: int | None = None
    best_lap_ms: int | None = None
    delta_ms: int | None = None
    sector_1_ms: int | None = None
    sector_2_ms: int | None = None
    sector_3_ms: int | None = None
    sector_1_delta_ms: int | None = None
    sector_2_delta_ms: int | None = None
    sector_3_delta_ms: int | None = None
    lap_number: int | None = None
    session_index: int | None = None
    session_type: int | None = None
    track_name: str | None = None
    car_model: str | None = None
    driver_name: str | None = None
    power_limited: bool = False
    power_limited_since_boot: bool = False
    power_status_available: bool = False
    throttled_flags: int | None = None
    received_at: str | None = None
    session_id: str | None = None
    session_active: bool = False
    session_started_at: str | None = None
    session_ended_at: str | None = None
    session_name: str | None = None
    upload_after_session: bool = False
    upload_state: str = "idle"
    sample_rate_hz: int | None = None
    schema_version: int | None = None
    last_sequence: int | None = None
    last_monotonic_us: int | None = None
    samples_received: int = 0
    packets_lost: int = 0
    packets_replayed: int = 0
    packets_auth_failed: int = 0
    packets_invalid: int = 0
    recording: bool = False
    recorded_samples: int = 0
    last_completed_lap_number: int | None = None
    last_completed_lap_valid: bool | None = None
    last_bundle_path: str | None = None
    last_manifest_path: str | None = None
    log_sequence: int = 0
    log_updated_at: str | None = None
    log_severity: str | None = None
    _lock: threading.RLock = field(default_factory=threading.RLock, init=False, repr=False)

    def update(self, **values: Any) -> None:
        with self._lock:
            for key, value in values.items():
                if value is not None or key in {
                    "rpm", "steering_angle", "g_x", "g_y", "g_z", "throttle", "brake",
                    "fuel", "tc", "abs_activity", "pit_limiter", "lap_position",
                    "wheel_speed_fl", "wheel_speed_fr", "wheel_speed_rl", "wheel_speed_rr",
                    "pressure_fl", "pressure_fr", "pressure_rl", "pressure_rr",
                    "core_temp_fl", "core_temp_fr", "core_temp_rl", "core_temp_rr",
                    "suspension_fl", "suspension_fr", "suspension_rl", "suspension_rr",
                    "damage_front", "damage_rear", "damage_left", "damage_right", "damage_center",
                    "best_lap_ms", "sector_1_ms", "sector_2_ms", "sector_3_ms",
                    "sector_1_delta_ms", "sector_2_delta_ms", "sector_3_delta_ms",
                }:
                    setattr(self, key, value)
            self.received_at = utcnow()

    def increment(self, **counters: int) -> None:
        """Increment receiver quality counters without exposing the state lock."""
        with self._lock:
            for key, amount in counters.items():
                setattr(self, key, int(getattr(self, key)) + int(amount))

    def note_log_activity(self, severity: str) -> None:
        """Expose a bounded dashboard notification without changing telemetry time."""
        with self._lock:
            self.log_sequence += 1
            self.log_updated_at = utcnow()
            self.log_severity = severity

    def clear_session(self) -> None:
        """Mark the current session ended while retaining its summary for the UI."""
        with self._lock:
            self.session_active = False
            self.session_ended_at = utcnow()
            self.recording = False

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                item.name: getattr(self, item.name)
                for item in fields(self)
                if not item.name.startswith("_")
            }
