"""Receive high-rate ACC shared-memory fields from the Windows companion."""
from __future__ import annotations

import json
import ipaddress
import logging
import math
import socket
import threading
import time

from .models import LiveState, utcnow

LOG = logging.getLogger(__name__)


class SharedMemoryReceiver:
    """Accept telemetry and heartbeat datagrams from the Windows daemon."""

    def __init__(self, source_host: str, port: int, state: LiveState, recorder=None, broker=None):
        self.source_host, self.port, self.state = source_host.strip(), port, state
        self.recorder = recorder
        self.broker = broker
        self._active_source_host: str | None = self.source_host or None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_packet_at: float | None = None
        self._timing_lap: int | None = None
        self._sector_cursor = 0
        self._sector_split_ms: list[int] = []
        self._best_sector_ms: list[int | None] = [None, None, None]
        self._best_lap_ms: int | None = None
        self._last_completion: tuple[int, int] | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="acc-shared-memory", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self.recorder:
            try:
                self.recorder.finish()
            except OSError:
                LOG.exception("could not finalize Pi telemetry log during shutdown")

    def _run(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(1)
            sock.bind(("0.0.0.0", self.port))
            while not self._stop.is_set():
                try:
                    payload, source = sock.recvfrom(8192)
                except socket.timeout:
                    self._expire_stale_data()
                    continue
                self.handle(payload, source[0])

    def handle(self, payload: bytes, source_host: str) -> bool:
        if self._active_source_host is not None and source_host != self._active_source_host:
            LOG.warning("ignoring companion packet from unexpected source %s", source_host)
            return False
        try:
            message = json.loads(payload)
            version = message.get("version")
            if version not in (1, 2, 3):
                raise ValueError("unsupported version")
            message_type = message.get("type", "telemetry")
            values = {}
            if message_type == "status":
                if version not in (2, 3):
                    raise ValueError("status heartbeat requires version 2 or 3")
                daemon_state = message["state"]
                if daemon_state not in {"waiting", "ready", "driving"}:
                    raise ValueError("invalid daemon state")
                simulator = message.get("simulator")
                if simulator is not None and simulator not in {"ACC", "AC", "ACE", "iRacing"}:
                    raise ValueError("invalid simulator")
            elif message_type == "telemetry":
                frame = message.get("telemetry", message)
                if version == 3 and frame is message:
                    raise ValueError("version 3 telemetry requires a full telemetry object")
                rpm_value = frame["rpm"]
                if (not isinstance(rpm_value, (int, float)) or isinstance(rpm_value, bool)
                        or not math.isfinite(rpm_value) or not 0 <= rpm_value <= 20_000):
                    raise ValueError("invalid rpm")
                rpm = int(rpm_value)
                values = {name: float(frame[name]) for name in ("steering_angle", "g_x", "g_y", "g_z")}
                if not all(math.isfinite(value) for value in values.values()):
                    raise ValueError("non-finite telemetry")
                for name in ("throttle", "brake"):
                    if name in frame:
                        value = float(frame[name])
                        if not math.isfinite(value) or not 0 <= value <= 1.001:
                            raise ValueError(f"invalid {name}")
                        values[name] = value
                daemon_state = "driving"
            else:
                raise ValueError("invalid message type")
            if version in (2, 3) and message_type == "telemetry":
                simulator = message["simulator"]
                if simulator not in {"ACC", "AC", "ACE", "iRacing"}:
                    raise ValueError("invalid simulator")
                optional = {
                    "gear": int, "speed_kmh": float, "current_lap_ms": int,
                    "completed_lap_ms": int, "delta_ms": int, "lap_number": int,
                    "track_name": str, "car_model": str, "driver_name": str,
                    "fuel": float, "tc": float, "abs_activity": float, "pit_limiter": bool,
                    "lap_position": float,
                    "wheel_speed_fl": float, "wheel_speed_fr": float,
                    "wheel_speed_rl": float, "wheel_speed_rr": float,
                    "pressure_fl": float, "pressure_fr": float,
                    "pressure_rl": float, "pressure_rr": float,
                    "core_temp_fl": float, "core_temp_fr": float,
                    "core_temp_rl": float, "core_temp_rr": float,
                    "suspension_fl": float, "suspension_fr": float,
                    "suspension_rl": float, "suspension_rr": float,
                    "damage_front": float, "damage_rear": float,
                    "damage_left": float, "damage_right": float, "damage_center": float,
                }
                for name, converter in optional.items():
                    source = frame if name in frame else message
                    if name in source and source[name] is not None:
                        values[name] = converter(source[name])
                values.update(simulator=simulator, connected=True)
            if not self.source_host:
                address = ipaddress.ip_address(source_host)
                if not (address.is_private or address.is_link_local or address.is_loopback):
                    raise ValueError("auto-discovery requires a private or link-local source")
        except (ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
            LOG.warning("discarded companion packet: %s", exc)
            return False
        if self._active_source_host is None:
            self._active_source_host = source_host
            LOG.info("learned companion source %s", source_host)
        self._last_packet_at = time.monotonic()
        common = dict(companion_connected=True, companion_daemon_state=daemon_state,
                      companion_source_host=source_host, companion_received_at=utcnow())
        if message_type == "status":
            self.state.simulator = simulator
            self.state.connected = self.state.acc_connected or daemon_state == "driving"
            self.state.update(**common)
            if self.recorder and daemon_state != "driving":
                try:
                    self.recorder.finish()
                except OSError:
                    LOG.exception("could not finalize Pi telemetry log")
        else:
            self._update_virtual_sectors(values)
            self.state.update(rpm=rpm, **common, **values)
            if self.recorder and version == 3:
                try:
                    self.recorder.record(message)
                except (OSError, ValueError, KeyError, TypeError):
                    LOG.exception("could not record Pi telemetry sample")
            if self.broker and version == 3:
                self.broker.publish_sample(
                    session_id=message.get("session_id"), sequence=message.get("sequence"),
                    monotonic_us=message.get("monotonic_us"), frame=frame,
                    packet_gap=int(message.get("_packet_gap", 0)),
                )
        return True

    def _update_virtual_sectors(self, values: dict) -> None:
        """Track lap thirds when canonical sector loops are unavailable.

        The companion emits a normalized lap position for every simulator.  These
        sectors are explicitly distance-third timing, not track timing-loop data.
        """
        lap_number = values.get("lap_number")
        current_ms = values.get("current_lap_ms")
        position = values.get("lap_position")
        completed_ms = values.get("completed_lap_ms")
        if isinstance(lap_number, int) and isinstance(completed_ms, int) and completed_ms > 0:
            token = (lap_number, completed_ms)
            if token != self._last_completion:
                self._last_completion = token
                if len(self._sector_split_ms) == 2:
                    self._record_sector(2, completed_ms - self._sector_split_ms[-1], values)
                if self._best_lap_ms is None or completed_ms < self._best_lap_ms:
                    self._best_lap_ms = completed_ms
                values["best_lap_ms"] = self._best_lap_ms
        if isinstance(lap_number, int) and lap_number != self._timing_lap:
            self._timing_lap = lap_number
            self._sector_cursor = 0
            self._sector_split_ms = []
        if isinstance(current_ms, int) and isinstance(position, (int, float)):
            normalized = float(position)
            if normalized > 1:
                normalized /= 100
            thresholds = (1 / 3, 2 / 3)
            if self._sector_cursor < len(thresholds) and normalized >= thresholds[self._sector_cursor]:
                elapsed_before = self._sector_split_ms[-1] if self._sector_split_ms else 0
                duration = current_ms - elapsed_before
                if duration > 0:
                    self._record_sector(self._sector_cursor, duration, values)
                    self._sector_split_ms.append(current_ms)
                    self._sector_cursor += 1

    def _record_sector(self, index: int, duration_ms: int, values: dict) -> None:
        if duration_ms <= 0:
            return
        best = self._best_sector_ms[index]
        delta = 0 if best is None else duration_ms - best
        if best is None or duration_ms < best:
            self._best_sector_ms[index] = duration_ms
        number = index + 1
        values[f"sector_{number}_ms"] = duration_ms
        values[f"sector_{number}_delta_ms"] = delta

    def _expire_stale_data(self) -> None:
        if self._last_packet_at is None or time.monotonic() - self._last_packet_at <= 1.5:
            return
        self.state.rpm = self.state.steering_angle = None
        self.state.g_x = self.state.g_y = self.state.g_z = None
        self.state.throttle = self.state.brake = None
        self.state.companion_connected = False
        self.state.companion_daemon_state = None
        self.state.companion_source_host = None
        if self.state.simulator != "ACC":
            self.state.connected = self.state.acc_connected
            if not self.state.acc_connected:
                self.state.simulator = None
        self._last_packet_at = None
        if self.recorder:
            try:
                self.recorder.finish()
            except OSError:
                LOG.exception("could not finalize stale Pi telemetry log")
        if not self.source_host:
            self._active_source_host = None
