import json
import time
import unittest

from rapid.bridge import SharedMemoryReceiver
from rapid.models import LiveState


class BridgeTests(unittest.TestCase):
    def test_valid_companion_packet_updates_live_state(self):
        state = LiveState()
        receiver = SharedMemoryReceiver("192.168.1.89", 9001, state)
        packet = json.dumps({
            "version": 1, "rpm": 6123, "steering_angle": -0.25,
            "g_x": 0.1, "g_y": 1.2, "g_z": -0.05,
        }).encode()
        self.assertTrue(receiver.handle(packet, "192.168.1.89"))
        self.assertEqual(state.rpm, 6123)
        self.assertTrue(state.companion_connected)

    def test_wrong_source_and_invalid_values_are_rejected(self):
        state = LiveState()
        receiver = SharedMemoryReceiver("192.168.1.89", 9001, state)
        packet = b'{"version":1,"rpm":99999,"steering_angle":0,"g_x":0,"g_y":0,"g_z":0}'
        self.assertFalse(receiver.handle(packet, "192.168.1.89"))
        self.assertFalse(receiver.handle(packet, "192.168.1.10"))
        self.assertIsNone(state.rpm)

    def test_v2_packet_updates_simulator_dashboard_fields(self):
        state = LiveState()
        receiver = SharedMemoryReceiver("192.168.1.89", 9001, state)
        packet = json.dumps({
            "version": 2, "simulator": "iRacing", "rpm": 7021,
            "steering_angle": 0.2, "g_x": 0.5, "g_y": 1.0, "g_z": -0.1,
            "gear": 4, "speed_kmh": 213.4, "current_lap_ms": 54321,
            "lap_number": 7, "track_name": "Spa", "car_model": "GT3",
        }).encode()
        self.assertTrue(receiver.handle(packet, "192.168.1.89"))
        self.assertEqual(state.simulator, "iRacing")
        self.assertTrue(state.connected)
        self.assertEqual(state.gear, 4)
        self.assertEqual(state.current_lap_ms, 54321)

    def test_empty_host_learns_one_private_sender_until_stale(self):
        state = LiveState()
        receiver = SharedMemoryReceiver("", 9001, state)
        packet = json.dumps({
            "version": 1, "rpm": 6000, "steering_angle": 0,
            "g_x": 0, "g_y": 1, "g_z": 0,
        }).encode()
        self.assertTrue(receiver.handle(packet, "192.168.50.20"))
        self.assertEqual(state.companion_source_host, "192.168.50.20")
        self.assertFalse(receiver.handle(packet, "192.168.50.21"))
        receiver._last_packet_at = time.monotonic() - 2
        receiver._expire_stale_data()
        self.assertTrue(receiver.handle(packet, "169.254.10.2"))
        self.assertEqual(state.companion_source_host, "169.254.10.2")

    def test_status_heartbeat_marks_daemon_connected_without_a_simulator(self):
        state = LiveState()
        receiver = SharedMemoryReceiver("", 9001, state)
        packet = b'{"version":2,"type":"status","state":"waiting","simulator":null}'
        self.assertTrue(receiver.handle(packet, "192.168.1.93"))
        self.assertTrue(state.companion_connected)
        self.assertEqual(state.companion_daemon_state, "waiting")
        self.assertIsNone(state.simulator)
        self.assertFalse(state.connected)

    def test_v3_full_frame_updates_inputs_and_is_sent_to_recorder(self):
        class Recorder:
            def __init__(self): self.messages = []
            def record(self, message): self.messages.append(message)
            def finish(self): pass

        recorder, state = Recorder(), LiveState()
        receiver = SharedMemoryReceiver("", 9001, state, recorder)
        packet = json.dumps({
            "version": 3, "type": "telemetry", "simulator": "ACC",
            "sample_rate_hz": 10, "track_name": "Spa", "car_model": "GT3",
            "telemetry": {
                "rpm": 6500.0, "steering_angle": -0.3, "g_x": 0.7,
                "g_y": 1.0, "g_z": -0.4, "throttle": 0.82,
                "brake": 0.15, "gear": 4, "speed_kmh": 198.2,
                "fuel": 46.5, "tc": 1, "abs_activity": 0.2, "pit_limiter": False,
                "pressure_fl": 27.1, "core_temp_fl": 82.4,
            },
        }).encode()
        self.assertTrue(receiver.handle(packet, "192.168.1.93"))
        self.assertEqual(state.rpm, 6500)
        self.assertAlmostEqual(state.throttle, 0.82)
        self.assertAlmostEqual(state.brake, 0.15)
        self.assertEqual(state.fuel, 46.5)
        self.assertEqual(state.tc, 1)
        self.assertAlmostEqual(state.pressure_fl, 27.1)
        self.assertAlmostEqual(state.core_temp_fl, 82.4)
        self.assertEqual(len(recorder.messages), 1)


if __name__ == "__main__":
    unittest.main()
