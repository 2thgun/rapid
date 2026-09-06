import struct
import unittest
from rapid.protocol import DecodeError, decode_packet, registration_packet

def s(value):
    raw = value.encode(); return struct.pack('<H', len(raw)) + raw
def lap(time=0, valid=True):
    return struct.pack('<iHHB', time, 1, 2, 3) + struct.pack('<iii', 0, 0, 0) + bytes([not valid, valid, 0, 0])

class ProtocolTests(unittest.TestCase):
    def test_registration_packet_v2(self):
        self.assertEqual(registration_packet('raPId', 'rapid', 250), struct.pack('<BB', 1, 2) + s('raPId') + s('rapid') + struct.pack('<iH', 250, 0))
    def test_registration_reply(self):
        kind, data = decode_packet(bytes([1]) + struct.pack('<iBB', 99, 1, 0) + s(''))
        self.assertEqual(kind, 1); self.assertTrue(data['success']); self.assertEqual(data['connection_id'], 99)
    def test_realtime_car_update(self):
        payload = bytes([3]) + struct.pack('<HHBBfffBHHHHfHi', 7, 2, 1, 4, 0, 0, 0, 1, 123, 2, 2, 2, 0, 4, -321) + lap(70000) + lap(87654) + lap(54321)
        _, data = decode_packet(payload)
        self.assertEqual(data['car_index'], 7); self.assertEqual(data['gear_raw'], 4); self.assertEqual(data['speed_kmh'], 123); self.assertEqual(data['rpm'], None); self.assertEqual(data['completed_lap_ms'], 87654)
    def test_rejects_bad_or_unknown_packets(self):
        with self.assertRaises(DecodeError): decode_packet(b'')
        with self.assertRaises(DecodeError): decode_packet(b'\x63')
        with self.assertRaises(DecodeError): decode_packet(b'\x01\x00')
