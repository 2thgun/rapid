from __future__ import annotations

import logging, socket, threading
from .config import Settings
from .models import LiveState
from .protocol import (DecodeError, ENTRY_LIST_CAR, REALTIME_CAR_UPDATE, REALTIME_UPDATE, TRACK_DATA, decode_packet, registration_packet)
from .storage import Store

LOG = logging.getLogger(__name__)

class AccClient:
    def __init__(self, settings: Settings, state: LiveState, store: Store):
        self.settings, self.state, self.store = settings, state, store; self._stop = threading.Event(); self._thread: threading.Thread | None = None
    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="acc-udp", daemon=True); self._thread.start()
    def stop(self) -> None:
        self._stop.set()
        if self._thread: self._thread.join(timeout=2)
    def _run(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            # ACC's broadcaster uses the registered UDP listener port for its
            # return stream; retain the established Pi-side port instead of an
            # ephemeral source port.
            sock.settimeout(1.0); sock.bind(("0.0.0.0", self.settings.acc_local_port))
            registration = registration_packet(self.settings.display_name, self.settings.acc_password, self.settings.update_interval_ms, self.settings.protocol_version)
            while not self._stop.is_set():
                try:
                    sock.sendto(registration, (self.settings.acc_host, self.settings.acc_port))
                    while not self._stop.is_set():
                        try: payload, source = sock.recvfrom(8192)
                        except socket.timeout: break
                        if source[0] != self.settings.acc_host:
                            LOG.warning("ignoring UDP packet from unexpected source %s", source[0]); continue
                        self.handle(payload)
                except OSError as exc: LOG.warning("ACC UDP error: %s", exc); self._stop.wait(2)
    def handle(self, payload: bytes) -> None:
        try: packet_type, data = decode_packet(payload)
        except DecodeError as exc: LOG.warning("discarded ACC packet: %s", exc); return
        self.store.record_packet(packet_type, data, self.state)
        if packet_type == 1:
            self.state.update(acc_connected=data["success"], connected=data["success"], simulator="ACC", connection_id=data["connection_id"]); return
        if packet_type == REALTIME_UPDATE:
            self.state.update(selected_car_index=data["focused_car_index"], session_index=data["session_index"], session_type=data["session_type"]); return
        if packet_type == TRACK_DATA: self.state.update(track_name=data["track_name"]); return
        if packet_type == ENTRY_LIST_CAR and data["car_index"] == self.state.selected_car_index:
            self.state.update(car_model=str(data["car_model"]))
            index, drivers = data["current_driver_index"], data["drivers"]
            if index < len(drivers):
                driver = drivers[index]
                name = f'{driver["first_name"]} {driver["last_name"]}'.strip()
                self.state.update(driver_name=name)
                self.store.upsert_driver(data["car_index"], name, driver["nationality"])
            return
        if packet_type == REALTIME_CAR_UPDATE and data["car_index"] == self.state.selected_car_index:
            # ACC gear values are encoded as reverse=0, neutral=1, first=2.
            gear = data["gear_raw"] - 1
            completed = data["completed_lap_ms"]
            # ACC represents an unavailable lap with Int32.MaxValue.
            if completed <= 0 or completed == 2_147_483_647:
                completed = None
            self.state.update(gear=gear, speed_kmh=data["speed_kmh"], current_lap_ms=data["current_lap_ms"], completed_lap_ms=completed, delta_ms=data["delta_ms"], lap_number=data["laps"])
            if completed is not None: self.store.write_completed_lap(self.state, data["last_lap"]["valid"])
