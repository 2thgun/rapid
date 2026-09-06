"""Disk-backed MoTeC LD recording for normalized companion telemetry."""
from __future__ import annotations

import logging
import hashlib
import json
import math
import os
import shutil
import struct
import threading
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import BinaryIO, Callable
from xml.sax.saxutils import escape

LOG = logging.getLogger(__name__)

HEADER_SIZE = 1762
EVENT_SIZE = 1154
CHANNEL_HEADER_SIZE = 124


@dataclass(frozen=True)
class Channel:
    name: str
    short_name: str
    unit: str
    key: str
    scale: float = 1.0


CHANNELS = (
    Channel("Time", "Time", "s", "elapsed"),
    Channel("Throttle Position", "Throttle", "%", "throttle", 100),
    Channel("Brake Position", "Brake", "%", "brake", 100),
    Channel("Fuel Level", "Fuel", "l", "fuel"),
    Channel("Gear", "Gear", "", "gear"),
    Channel("Engine RPM", "RPM", "rpm", "rpm"),
    Channel("Steering Position", "Steer", "rad", "steering_angle"),
    Channel("Ground Speed", "Speed", "km/h", "speed_kmh"),
    Channel("Velocity X", "Vel X", "m/s", "velocity_x"),
    Channel("Velocity Y", "Vel Y", "m/s", "velocity_y"),
    Channel("Velocity Z", "Vel Z", "m/s", "velocity_z"),
    Channel("G Force X", "G X", "g", "g_x"),
    Channel("G Force Y", "G Y", "g", "g_y"),
    Channel("G Force Z", "G Z", "g", "g_z"),
    Channel("Wheel Slip FL", "Slip FL", "", "wheel_slip_fl"),
    Channel("Wheel Slip FR", "Slip FR", "", "wheel_slip_fr"),
    Channel("Wheel Slip RL", "Slip RL", "", "wheel_slip_rl"),
    Channel("Wheel Slip RR", "Slip RR", "", "wheel_slip_rr"),
    Channel("Tyre Pressure FL", "Press FL", "psi", "pressure_fl"),
    Channel("Tyre Pressure FR", "Press FR", "psi", "pressure_fr"),
    Channel("Tyre Pressure RL", "Press RL", "psi", "pressure_rl"),
    Channel("Tyre Pressure RR", "Press RR", "psi", "pressure_rr"),
    Channel("Wheel Speed FL", "WhlSp FL", "rad/s", "wheel_speed_fl"),
    Channel("Wheel Speed FR", "WhlSp FR", "rad/s", "wheel_speed_fr"),
    Channel("Wheel Speed RL", "WhlSp RL", "rad/s", "wheel_speed_rl"),
    Channel("Wheel Speed RR", "WhlSp RR", "rad/s", "wheel_speed_rr"),
    Channel("Tyre Core Temp FL", "Core FL", "C", "core_temp_fl"),
    Channel("Tyre Core Temp FR", "Core FR", "C", "core_temp_fr"),
    Channel("Tyre Core Temp RL", "Core RL", "C", "core_temp_rl"),
    Channel("Tyre Core Temp RR", "Core RR", "C", "core_temp_rr"),
    Channel("Suspension Travel FL", "Susp FL", "m", "suspension_fl"),
    Channel("Suspension Travel FR", "Susp FR", "m", "suspension_fr"),
    Channel("Suspension Travel RL", "Susp RL", "m", "suspension_rl"),
    Channel("Suspension Travel RR", "Susp RR", "m", "suspension_rr"),
    Channel("TC Activity", "TC", "", "tc"),
    Channel("Heading", "Heading", "rad", "heading"),
    Channel("Pitch", "Pitch", "rad", "pitch"),
    Channel("Roll", "Roll", "rad", "roll"),
    Channel("Damage Front", "Dmg F", "", "damage_front"),
    Channel("Damage Rear", "Dmg R", "", "damage_rear"),
    Channel("Damage Left", "Dmg L", "", "damage_left"),
    Channel("Damage Right", "Dmg Rgt", "", "damage_right"),
    Channel("Damage Center", "Dmg C", "", "damage_center"),
    Channel("Pit Limiter", "Pit Lim", "", "pit_limiter"),
    Channel("ABS Activity", "ABS", "", "abs"),
    Channel("Lap Number", "Lap", "", "lap_number"),
    Channel("Lap Time", "Lap Time", "s", "current_lap_ms", 0.001),
    Channel("Lap Position", "Lap Pos", "%", "lap_position", 100),
)


@dataclass(frozen=True)
class LapSegment:
    number: int
    start_sample: int
    end_sample: int
    time_ms: int
    valid: bool | None = None


@dataclass(frozen=True)
class FinalizedBundle(os.PathLike[str]):
    """Published session bundle; path-like compatibility points at the full LD."""

    directory: Path
    manifest_path: Path
    full_session_path: Path
    ldx_path: Path
    lap_paths: tuple[Path, ...]
    manifest: dict

    def __fspath__(self) -> str:
        return os.fspath(self.full_session_path)


def _fixed(value: object, length: int) -> bytes:
    raw = str(value or "").encode("ascii", "replace")[:length]
    return raw.ljust(length, b"\0")


def _safe_name(value: object, fallback: str) -> str:
    safe = "".join(char if char.isalnum() or char in "-_" else "_" for char in str(value or ""))
    return safe.strip("_") or fallback


class MotecRecorder:
    """Authoritative disk-backed recorder with atomic, recoverable publication."""

    def __init__(
        self,
        output_directory: str | Path,
        on_bundle_finalized: Callable[[FinalizedBundle], None] | None = None,
    ):
        self.output_directory = Path(output_directory)
        self._lock = threading.RLock()
        self._spool_directory: Path | None = None
        self._streams: list[BinaryIO] = []
        self._metadata: dict[str, object] = {}
        self._sample_rate = 10
        self._sample_count = 0
        self._received_count = 0
        self._last_frame: dict[str, object] | None = None
        self._last_lap_number: int | None = None
        self._lap_start_sample = 0
        self._last_completed_token: tuple[int, int] | None = None
        self._laps: list[LapSegment] = []
        self._quality: dict[str, int] = {}
        self._availability: dict[str, int] = {}
        self._upload_after_session = False
        self._callback = on_bundle_finalized
        self.last_path: Path | None = None
        self.last_bundle: FinalizedBundle | None = None
        self.recovered_bundles: list[FinalizedBundle] = []
        self.output_directory.mkdir(parents=True, exist_ok=True)
        self._recover_spools()

    @property
    def recording(self) -> bool:
        return bool(self._streams)

    @property
    def sample_count(self) -> int:
        return self._sample_count

    @property
    def session_id(self) -> str | None:
        return str(self._metadata.get("session_id")) if self._metadata else None

    @property
    def upload_after_session(self) -> bool:
        return self._upload_after_session

    def set_bundle_callback(self, callback: Callable[[FinalizedBundle], None] | None) -> None:
        self._callback = callback

    def set_upload_after_session(self, enabled: bool) -> None:
        with self._lock:
            self._upload_after_session = bool(enabled)
            if self.recording:
                self._checkpoint(force=True)

    def status(self) -> dict[str, object]:
        with self._lock:
            return {
                "recording": self.recording,
                "session_id": self.session_id,
                "sample_rate_hz": self._sample_rate if self.recording else None,
                "received_samples": self._received_count,
                "recorded_samples": self._sample_count,
                "completed_laps": len(self._laps),
                "upload_after_session": self._upload_after_session,
                "last_bundle_path": str(self.last_bundle.directory) if self.last_bundle else None,
            }

    def record(self, message: dict) -> None:
        frame = message["telemetry"]
        simulator = str(message["simulator"])
        sample_rate = max(1, min(100, int(message.get("sample_rate_hz", 10))))
        session_id = str(message.get("session_id") or message.get("run_id") or "")
        with self._lock:
            if self.recording and (
                simulator != self._metadata["simulator"]
                or sample_rate != self._sample_rate
                or (session_id and session_id != self._metadata["session_id"])
            ):
                self.finish(reason="session_changed")
            if not self.recording:
                self._start(message, simulator, sample_rate, session_id or uuid.uuid4().hex)
            self._refresh_metadata(message)
            gap = max(0, int(message.get("_packet_gap", 0)))
            self._received_count += 1
            if gap:
                self._quality["gap_events"] += 1
                self._quality["missing_packets"] += gap
                substitute = min(gap, self._sample_rate * 5)
                if self._last_frame is not None:
                    for _ in range(substitute):
                        self._write_frame(self._last_frame, substituted=True)
                self._quality["unfilled_missing_packets"] += gap - substitute
            self._detect_lap(frame, message)
            self._write_frame(frame, substituted=False)
            self._last_frame = dict(frame)
            if self._sample_count % self._sample_rate == 0:
                self._checkpoint(force=True)

    def _start(self, message: dict, simulator: str, sample_rate: int, session_id: str) -> None:
        self._spool_directory = self.output_directory / f".rapid-spool-{uuid.uuid4().hex}"
        self._spool_directory.mkdir()
        try:
            self._streams = [
                (self._spool_directory / f"{index:02d}.bin").open("wb", buffering=64 * 1024)
                for index in range(len(CHANNELS))
            ]
        except Exception:
            self._close_streams()
            shutil.rmtree(self._spool_directory, ignore_errors=True)
            self._spool_directory = None
            raise
        started_value = message.get("started_at")
        try:
            started = datetime.fromisoformat(str(started_value)) if started_value else datetime.now().astimezone()
        except ValueError:
            started = datetime.now().astimezone()
        self._metadata = {
            "session_id": session_id,
            "started_at": started,
            "simulator": simulator,
            "driver": message.get("driver_name", ""),
            "vehicle": message.get("car_model", ""),
            "venue": message.get("track_name", ""),
            "session": message.get("session_name", ""),
            "schema_version": int(message.get("schema_version", 1)),
            "run_id": str(message.get("run_id", "")),
        }
        session_name = str(self._metadata["session"]).strip().casefold()
        if "upload_after_session" in message:
            self._upload_after_session = bool(message["upload_after_session"])
        else:
            self._upload_after_session = session_name == "race"
        self._sample_rate = sample_rate
        self._sample_count = self._received_count = 0
        self._last_frame = None
        self._last_lap_number = None
        self._lap_start_sample = 0
        self._last_completed_token = None
        self._laps = []
        self._quality = {
            "gap_events": 0,
            "missing_packets": 0,
            "substituted_samples": 0,
            "unfilled_missing_packets": 0,
            "invalid_samples": 0,
        }
        self._availability = {channel.key: 0 for channel in CHANNELS if channel.key != "elapsed"}
        self._checkpoint(force=True)
        LOG.info("recording %s telemetry at %s Hz (session %s)", simulator, sample_rate, session_id)

    def _refresh_metadata(self, message: dict) -> None:
        for source, target in (
            ("driver_name", "driver"), ("car_model", "vehicle"),
            ("track_name", "venue"), ("session_name", "session"),
        ):
            value = message.get(source)
            if value:
                self._metadata[target] = str(value)

    def _write_frame(self, frame: dict[str, object], *, substituted: bool) -> None:
        for stream, channel in zip(self._streams, CHANNELS):
            raw_value = self._sample_count / self._sample_rate if channel.key == "elapsed" else frame.get(channel.key, 0)
            try:
                value = float(raw_value) * channel.scale
            except (TypeError, ValueError):
                value = 0.0
                self._quality["invalid_samples"] += 1
            if not math.isfinite(value):
                value = 0.0
                self._quality["invalid_samples"] += 1
            elif channel.key != "elapsed" and channel.key in frame:
                self._availability[channel.key] += 1
            stream.write(struct.pack("<f", value))
        self._sample_count += 1
        if substituted:
            self._quality["substituted_samples"] += 1

    def _detect_lap(self, frame: dict, message: dict) -> None:
        try:
            lap_number = int(float(frame.get("lap_number", 0)))
        except (TypeError, ValueError):
            lap_number = 0
        try:
            completed_ms = int(message.get("completed_lap_ms") or frame.get("completed_lap_ms") or 0)
        except (TypeError, ValueError):
            completed_ms = 0
        valid_value = message.get("lap_valid", frame.get("lap_valid"))
        valid = valid_value if isinstance(valid_value, bool) else None
        if self._last_lap_number is None:
            self._last_lap_number = lap_number
            self._lap_start_sample = self._sample_count
            return
        advanced = lap_number > self._last_lap_number
        token = (lap_number, completed_ms)
        completion_changed = completed_ms > 0 and token != self._last_completed_token
        if (advanced or completion_changed) and completed_ms > 0 and self._sample_count > self._lap_start_sample:
            number = lap_number - 1 if advanced else self._last_lap_number
            self._laps.append(LapSegment(
                number=max(0, number), start_sample=self._lap_start_sample,
                end_sample=self._sample_count, time_ms=completed_ms, valid=valid,
            ))
            self._lap_start_sample = self._sample_count
            self._last_completed_token = token
            self._checkpoint(force=True)
        if advanced:
            self._last_lap_number = lap_number

    def finish(self, reason: str = "ended") -> FinalizedBundle | None:
        with self._lock:
            if not self.recording:
                return None
            self._checkpoint(force=True)
            self._close_streams()
            spool = self._spool_directory
            self._spool_directory = None
            if self._sample_count == 0 or spool is None:
                if spool:
                    shutil.rmtree(spool, ignore_errors=True)
                return None
            metadata = dict(self._metadata)
            laps = list(self._laps)
            quality = dict(self._quality)
            availability = dict(self._availability)
            try:
                bundle = self._publish_spool(
                    spool, metadata, self._sample_count, self._received_count,
                    self._sample_rate, laps, quality, availability,
                    self._upload_after_session, reason=reason,
                )
            finally:
                shutil.rmtree(spool, ignore_errors=True)
                self._reset_runtime()
            self.last_bundle = bundle
            self.last_path = bundle.full_session_path
            self._notify(bundle)
            LOG.info("published %s samples to %s", bundle.manifest["quality"]["recorded_samples"], bundle.directory)
            return bundle

    def _reset_runtime(self) -> None:
        self._metadata = {}
        self._sample_count = self._received_count = 0
        self._last_frame = None
        self._last_lap_number = None
        self._laps = []

    def _close_streams(self) -> None:
        streams, self._streams = self._streams, []
        for stream in streams:
            stream.close()

    def _checkpoint(self, force: bool = False) -> None:
        if not self._spool_directory:
            return
        if force:
            for stream in self._streams:
                stream.flush()
                os.fsync(stream.fileno())
        state = {
            "metadata": {
                **self._metadata,
                "started_at": self._metadata["started_at"].isoformat(),
            },
            "sample_rate_hz": self._sample_rate,
            "sample_count": self._sample_count,
            "received_count": self._received_count,
            "laps": [asdict(lap) for lap in self._laps],
            "quality": self._quality,
            "availability": self._availability,
            "upload_after_session": self._upload_after_session,
        }
        target = self._spool_directory / "spool.json"
        temporary = self._spool_directory / ".spool.json.tmp"
        with temporary.open("w", encoding="utf-8", newline="\n") as handle:
            json.dump(state, handle, separators=(",", ":"), sort_keys=True)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, target)

    def _recover_spools(self) -> None:
        for spool in sorted(self.output_directory.glob(".rapid-spool-*")):
            try:
                state = json.loads((spool / "spool.json").read_text(encoding="utf-8"))
                sizes = [(spool / f"{index:02d}.bin").stat().st_size // 4 for index in range(len(CHANNELS))]
                sample_count = min([int(state.get("sample_count", 0)), *sizes])
                if sample_count <= 0:
                    raise ValueError("spool has no complete samples")
                metadata = dict(state["metadata"])
                metadata["started_at"] = datetime.fromisoformat(metadata["started_at"])
                laps = [
                    LapSegment(**item) for item in state.get("laps", [])
                    if int(item.get("end_sample", 0)) <= sample_count
                ]
                quality = {key: int(value) for key, value in state.get("quality", {}).items()}
                quality["recovered_after_restart"] = 1
                bundle = self._publish_spool(
                    spool, metadata, sample_count, int(state.get("received_count", sample_count)),
                    int(state["sample_rate_hz"]), laps, quality,
                    {key: int(value) for key, value in state.get("availability", {}).items()},
                    bool(state.get("upload_after_session", False)), reason="recovered_after_restart",
                )
                self.recovered_bundles.append(bundle)
                self.last_bundle, self.last_path = bundle, bundle.full_session_path
                self._notify(bundle)
                shutil.rmtree(spool, ignore_errors=True)
                LOG.warning("recovered interrupted telemetry session into %s", bundle.directory)
            except Exception:
                quarantine = self.output_directory / f".rapid-incomplete-{spool.name.removeprefix('.rapid-spool-')}"
                try:
                    if not quarantine.exists():
                        os.replace(spool, quarantine)
                except OSError:
                    pass
                LOG.exception("could not recover telemetry spool %s; preserved as %s", spool, quarantine)

    def _publish_spool(
        self,
        spool: Path,
        metadata: dict[str, object],
        sample_count: int,
        received_count: int,
        sample_rate: int,
        laps: list[LapSegment],
        quality: dict[str, int],
        availability: dict[str, int],
        upload_after_session: bool,
        *,
        reason: str,
    ) -> FinalizedBundle:
        started = metadata["started_at"]
        if not isinstance(started, datetime):
            started = datetime.fromisoformat(str(started))
            metadata["started_at"] = started
        simulator = str(metadata["simulator"])
        venue = _safe_name(metadata.get("venue"), simulator)
        session_id = _safe_name(metadata.get("session_id"), uuid.uuid4().hex)
        stem = f"{started:%Y-%m-%d_%H-%M-%S}_{simulator}_{venue}_{session_id[:12]}"
        destination = self.output_directory / stem
        suffix = 1
        while destination.exists():
            destination = self.output_directory / f"{stem}_{suffix}"
            suffix += 1
        staging = self.output_directory / f".rapid-publish-{uuid.uuid4().hex}"
        staging.mkdir()
        try:
            full_ld = staging / "full-session.ld"
            self._write_ld(spool, full_ld, metadata, sample_rate, 0, sample_count)
            full_ldx = staging / "full-session.ldx"
            self._write_ldx(full_ldx, laps, sample_rate)
            laps_dir = staging / "laps"
            laps_dir.mkdir()
            lap_paths: list[Path] = []
            for lap in laps:
                validity = "valid" if lap.valid is True else "invalid" if lap.valid is False else "unknown"
                path = laps_dir / f"lap-{lap.number:03d}-{validity}.ld"
                self._write_ld(spool, path, metadata, sample_rate, lap.start_sample, lap.end_sample)
                lap_paths.append(path)
            artifacts: list[dict[str, object]] = []
            for artifact_id, role, path, lap_number in [
                ("full-ld", "full_ld", full_ld, None),
                ("full-ldx", "ldx", full_ldx, None),
                *[(f"lap-{lap.number:03d}", "lap_ld", path, lap.number) for lap, path in zip(laps, lap_paths)],
            ]:
                item: dict[str, object] = {
                    "id": artifact_id,
                    "role": role,
                    "relative_path": path.relative_to(staging).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": _sha256(path),
                }
                if lap_number is not None:
                    item["lap_number"] = lap_number
                artifacts.append(item)
            ended = datetime.now(timezone.utc)
            manifest = {
                "format_version": 1,
                "session_id": str(metadata["session_id"]),
                "simulator": simulator,
                "track": str(metadata.get("venue", "")),
                "started_at": started.astimezone(timezone.utc).isoformat(),
                "ended_at": ended.isoformat(),
                "metadata": {
                    "driver": str(metadata.get("driver", "")),
                    "vehicle": str(metadata.get("vehicle", "")),
                    "venue": str(metadata.get("venue", "")),
                    "session": str(metadata.get("session", "")),
                    "run_id": str(metadata.get("run_id", "")),
                    "schema_version": int(metadata.get("schema_version", 1)),
                    "sample_rate_hz": sample_rate,
                    "upload_after_session": upload_after_session,
                    "finish_reason": reason,
                },
                "quality": {
                    **quality,
                    "received_samples": received_count,
                    "recorded_samples": sample_count,
                    "channel_available_samples": availability,
                },
                "laps": [asdict(lap) for lap in laps],
                "artifacts": artifacts,
            }
            manifest_path = staging / "manifest.json"
            with manifest_path.open("w", encoding="utf-8", newline="\n") as handle:
                json.dump(manifest, handle, indent=2, sort_keys=True)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(staging, destination)
            return FinalizedBundle(
                directory=destination,
                manifest_path=destination / "manifest.json",
                full_session_path=destination / "full-session.ld",
                ldx_path=destination / "full-session.ldx",
                lap_paths=tuple(destination / path.relative_to(staging) for path in lap_paths),
                manifest=manifest,
            )
        finally:
            if staging.exists():
                shutil.rmtree(staging, ignore_errors=True)

    def _write_ld(
        self,
        spool: Path,
        destination: Path,
        metadata: dict[str, object],
        sample_rate: int,
        start_sample: int,
        end_sample: int,
    ) -> None:
        count = max(0, end_sample - start_sample)
        temporary = destination.with_name(f".{destination.name}.{uuid.uuid4().hex}.part")
        try:
            with temporary.open("wb") as output:
                self._write_headers(output, metadata, sample_rate, count)
                for index, channel in enumerate(CHANNELS):
                    if channel.key == "elapsed" and start_sample:
                        for sample in range(count):
                            output.write(struct.pack("<f", sample / sample_rate))
                        continue
                    with (spool / f"{index:02d}.bin").open("rb") as source:
                        source.seek(start_sample * 4)
                        remaining = count * 4
                        while remaining:
                            block = source.read(min(64 * 1024, remaining))
                            if not block:
                                raise OSError("telemetry spool ended before declared sample count")
                            output.write(block)
                            remaining -= len(block)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, destination)
        finally:
            if temporary.exists():
                temporary.unlink()

    def _write_ldx(self, destination: Path, laps: list[LapSegment], sample_rate: int) -> None:
        beacons = sorted({lap.start_sample for lap in laps} | {lap.end_sample for lap in laps})
        complete = [lap for lap in laps if lap.time_ms > 0]
        fastest = min(complete, key=lambda lap: lap.time_ms) if complete else None
        lines = [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<LDXFile Version="1.6" Locale="English" DefaultLocale="C">',
            ' <Layers>', '  <Layer>', '   <MarkerBlock>',
            '    <MarkerGroup Name="Beacons" Index="3">',
        ]
        for index, sample in enumerate(beacons, 1):
            microseconds = sample * 1_000_000 / sample_rate
            lines.append(
                f'     <Marker Version="100" ClassName="BCN" Name="raPId.{index}" '
                f'Flags="77" Time="{microseconds:.6f}"/>'
            )
        lines.extend([
            '    </MarkerGroup>', '   </MarkerBlock>', '   <RangeBlock/>',
            '  </Layer>', '  <Details>',
            f'   <String Id="Total Laps" Value="{len(complete)}"/>',
        ])
        if fastest:
            minutes, remainder = divmod(fastest.time_ms, 60_000)
            seconds = remainder / 1000
            lines.append(f'   <String Id="Fastest Time" Value="{minutes}:{seconds:06.3f}"/>')
            lines.append(f'   <String Id="Fastest Lap" Value="{fastest.number}"/>')
        lines.extend(['  </Details>', ' </Layers>', '</LDXFile>', ''])
        with destination.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines))
            handle.flush()
            os.fsync(handle.fileno())

    def _write_headers(
        self,
        output: BinaryIO,
        metadata: dict[str, object],
        sample_rate: int,
        sample_count: int,
    ) -> None:
        event_pointer = HEADER_SIZE
        metadata_pointer = HEADER_SIZE + EVENT_SIZE
        data_pointer = metadata_pointer + len(CHANNELS) * CHANNEL_HEADER_SIZE
        write = output.write
        write(struct.pack("<I", 0x40)); write(bytes(4))
        write(struct.pack("<II", metadata_pointer, data_pointer))
        write(bytes(20)); write(struct.pack("<I", event_pointer)); write(bytes(24))
        write(struct.pack("<HHHI", 1, 0x4240, 0x000F, 0x1F44)); write(_fixed("ADL", 8))
        write(struct.pack("<HHI", 420, 0xADB0, len(CHANNELS))); write(bytes(4))
        started = metadata["started_at"]
        write(_fixed(started.strftime("%d/%m/%Y"), 16)); write(bytes(16))
        write(_fixed(started.strftime("%H:%M:%S"), 16)); write(bytes(16))
        write(_fixed(metadata["driver"], 64)); write(_fixed(metadata["vehicle"], 64))
        write(bytes(64)); write(_fixed(metadata["venue"], 64)); write(bytes(64 + 1024))
        write(struct.pack("<I", 0x000C81A4)); write(bytes(66))
        write(_fixed(f"raPId {metadata['simulator']} telemetry", 64)); write(bytes(126))
        if output.tell() != HEADER_SIZE:
            raise RuntimeError("invalid LD header size")
        write(_fixed(metadata["simulator"], 64)); write(_fixed(metadata["session"], 64))
        write(_fixed("Recorded by raPId", 1024)); write(struct.pack("<H", 0))
        if output.tell() != metadata_pointer:
            raise RuntimeError("invalid LD event size")
        next_data_pointer = data_pointer
        for index, channel in enumerate(CHANNELS):
            previous = 0 if index == 0 else metadata_pointer + (index - 1) * CHANNEL_HEADER_SIZE
            following = 0 if index == len(CHANNELS) - 1 else metadata_pointer + (index + 1) * CHANNEL_HEADER_SIZE
            write(struct.pack("<IIIIHHHHhhhh", previous, following, next_data_pointer,
                              sample_count, 0x2EE1 + index, 0x07, 4,
                              sample_rate, 0, 1, 1, 0))
            write(_fixed(channel.name, 32)); write(_fixed(channel.short_name, 8))
            write(_fixed(channel.unit, 12)); write(bytes(40))
            next_data_pointer += sample_count * 4
        if output.tell() != data_pointer:
            raise RuntimeError("invalid LD channel metadata size")

    def _notify(self, bundle: FinalizedBundle) -> None:
        if not self._callback:
            return
        try:
            self._callback(bundle)
        except Exception:
            LOG.exception("bundle-finalized callback failed for %s", bundle.directory)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
