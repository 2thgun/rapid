"""Minimal defensive decoder for ACC broadcasting protocol v2 packets.

All fields are read in wire order using the .NET BinaryReader conventions used by
the ACC SDK (little endian primitives and 7-bit-prefixed UTF-8 strings).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any

REGISTRATION_RESULT = 1
REALTIME_UPDATE = 2
REALTIME_CAR_UPDATE = 3
ENTRY_LIST = 4
TRACK_DATA = 5
ENTRY_LIST_CAR = 6


class DecodeError(ValueError):
    pass


class Reader:
    def __init__(self, payload: bytes): self.payload, self.pos = payload, 0
    def _take(self, size: int) -> bytes:
        if self.pos + size > len(self.payload): raise DecodeError("truncated packet")
        result = self.payload[self.pos:self.pos + size]; self.pos += size; return result
    def u8(self) -> int: return self._take(1)[0]
    def bool(self) -> bool: return bool(self.u8())
    def u16(self) -> int: return struct.unpack("<H", self._take(2))[0]
    def i16(self) -> int: return struct.unpack("<h", self._take(2))[0]
    def i32(self) -> int: return struct.unpack("<i", self._take(4))[0]
    def f32(self) -> float: return struct.unpack("<f", self._take(4))[0]
    def string(self) -> str:
        length = 0; shift = 0
        while True:
            byte = self.u8(); length |= (byte & 0x7f) << shift
            if not byte & 0x80: break
            shift += 7
            if shift > 28: raise DecodeError("invalid string length")
        return self._take(length).decode("utf-8", errors="replace")


def _lap(reader: Reader) -> dict[str, Any]:
    """Read ACC's variable-length LapInfo structure."""
    lap_time, car_index, driver_index, split_count = reader.i32(), reader.u16(), reader.u16(), reader.u8()
    if split_count > 8: raise DecodeError("invalid lap split count")
    splits = [reader.i32() for _ in range(split_count)]
    is_invalid, valid_for_best, is_outlap, is_inlap = reader.bool(), reader.bool(), reader.bool(), reader.bool()
    return {"lap_time_ms": lap_time, "car_index": car_index, "driver_index": driver_index, "splits": splits,
            "valid": not is_invalid, "valid_for_best": valid_for_best, "is_outlap": is_outlap, "is_inlap": is_inlap}


def registration_packet(display_name: str, password: str, interval_ms: int, protocol_version: int = 2) -> bytes:
    def acc_string(value: str) -> bytes:
        body = value.encode("utf-8")
        if len(body) > 65535: raise ValueError("ACC string exceeds 65535 byte protocol limit")
        # The existing Pi handshake was verified with uint16 string lengths.
        return struct.pack("<H", len(body)) + body
    return struct.pack("<BB", REGISTRATION_RESULT, protocol_version) + acc_string(display_name) + acc_string(password) + struct.pack("<iH", interval_ms, 0)


def decode_packet(payload: bytes) -> tuple[int, dict[str, Any]]:
    if not payload: raise DecodeError("empty datagram")
    reader = Reader(payload); packet_type = reader.u8()
    if packet_type == REGISTRATION_RESULT:
        # The Stage 1 client needs only these stable v2 fields.  Do not consume
        # optional build-specific trailing data after a successful registration.
        return packet_type, {"connection_id": reader.i32(), "success": reader.bool()}
    if packet_type == REALTIME_UPDATE:
        # These leading fields are stable; later versions append fields, which
        # are deliberately not required to select and display the focused car.
        return packet_type, {"event_index": reader.i16(), "session_index": reader.i16(), "session_type": reader.u8(), "phase": reader.u8(),
            "session_time_ms": reader.i32(), "remaining_time_ms": reader.i32(), "focused_car_index": reader.i32()}
    if packet_type == REALTIME_CAR_UPDATE:
        result = {"car_index": reader.u16(), "driver_index": reader.u16(), "driver_count": reader.u8(), "gear_raw": reader.u8()}
        result.update(world_pos_x=reader.f32(), world_pos_y=reader.f32(), yaw=reader.f32(), car_location=reader.u8(),
                      speed_kmh=reader.u16(), position=reader.u16(), cup_position=reader.u16(), track_position=reader.u16(),
                      track_relative_position=reader.f32(), laps=reader.u16(), delta_ms=reader.i32(),
                      best_session_lap=_lap(reader), last_lap=_lap(reader), current_lap=_lap(reader))
        result["current_lap_ms"] = result["current_lap"]["lap_time_ms"]
        result["completed_lap_ms"] = result["last_lap"]["lap_time_ms"]
        # RPM is shared-memory-only; never reinterpret trailing fields as RPM.
        result["rpm"] = None
        return packet_type, result
    if packet_type == ENTRY_LIST:
        return packet_type, {"connection_id": reader.i32(), "car_indices": [reader.u16() for _ in range(reader.u16())]}
    if packet_type == TRACK_DATA:
        return packet_type, {"connection_id": reader.i32(), "track_name": reader.string(), "track_id": reader.i32(), "track_meters": reader.i32()}
    if packet_type == ENTRY_LIST_CAR:
        connection_id, car_index = reader.i32(), reader.u16()
        result = {"connection_id": connection_id, "car_index": car_index, "car_model": reader.u8(), "team_name": reader.string(), "race_number": reader.i32(), "cup_category": reader.u8(), "current_driver_index": reader.u8(), "nationality": reader.u16(), "drivers": []}
        if reader.pos < len(payload):
            count = reader.u8()
            for _ in range(count):
                result["drivers"].append({"first_name": reader.string(), "last_name": reader.string(), "short_name": reader.string(), "category": reader.u8(), "nationality": reader.u16()})
        return packet_type, result
    raise DecodeError(f"unknown packet type {packet_type}")
