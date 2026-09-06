import struct
import tempfile
import unittest
from pathlib import Path

from rapid.config import Settings
from rapid.models import LiveState
from rapid.service import AccClient
from rapid.storage import Store

def lap(time=0):
    return struct.pack('<iHHB', time, 1, 2, 0) + bytes([0, 1, 0, 0])

class ClientTests(unittest.TestCase):
    def test_car_update_for_unselected_car_is_ignored(self):
        packet = bytes([3]) + struct.pack('<HHBBfffBHHHHfHi', 7, 2, 1, 4, 0, 0, 0, 1, 123, 2, 2, 2, 0, 4, -321) + lap() + lap(87654) + lap(54321)
        with tempfile.TemporaryDirectory() as directory:
            state = LiveState(selected_car_index=99)
            store = Store(Path(directory) / 'rapid.db')
            AccClient(Settings(database_path=Path(directory) / 'rapid.db'), state, store).handle(packet)
            self.assertIsNone(state.speed_kmh)
            self.assertEqual(store.connection.execute('SELECT COUNT(*) FROM laps').fetchone()[0], 0)
            store.close()
