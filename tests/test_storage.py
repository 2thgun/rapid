import tempfile
import unittest
from pathlib import Path
from rapid.models import LiveState
from rapid.storage import Store

class StorageTests(unittest.TestCase):
    def test_completed_lap_is_persisted_once(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / 'rapid.db')
            state = LiveState(selected_car_index=4, session_index=2, track_name='Spa', car_model='GT3', lap_number=1, completed_lap_ms=123456, delta_ms=-120)
            self.assertTrue(store.write_completed_lap(state, True)); self.assertFalse(store.write_completed_lap(state, True))
            row = store.connection.execute('SELECT lap_time_ms, is_valid, sync_status FROM laps').fetchone()
            self.assertEqual(tuple(row), (123456, 1, 'pending'))
            store.record_packet(3, {'car_index': 4, 'speed_kmh': 100}, state)
            self.assertEqual(store.connection.execute('SELECT COUNT(*) FROM telemetry_packets').fetchone()[0], 1)
            store.upsert_driver(4, 'Test Driver', 1)
            self.assertEqual(store.connection.execute('SELECT name FROM drivers').fetchone()[0], 'Test Driver')
            store.close()
            restarted = Store(Path(directory) / 'rapid.db')
            self.assertFalse(restarted.write_completed_lap(state, True))
            restarted.close()
