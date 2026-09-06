import struct
import tempfile
import unittest
from pathlib import Path

from rapid.motec import CHANNELS, CHANNEL_HEADER_SIZE, EVENT_SIZE, HEADER_SIZE, MotecRecorder


class MotecRecorderTests(unittest.TestCase):
    def test_full_frames_are_written_as_structurally_valid_ld(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = MotecRecorder(directory)
            message = {
                "version": 3, "type": "telemetry", "simulator": "ACC",
                "sample_rate_hz": 10, "driver_name": "Driver", "car_model": "GT3",
                "track_name": "Spa", "session_name": "Race",
                "telemetry": {
                    "rpm": 6000, "throttle": 0.5, "brake": 0.1,
                    "steering_angle": -0.2, "g_x": 0.4, "g_y": 1.0,
                    "g_z": -0.1, "gear": 3, "speed_kmh": 160,
                },
            }
            recorder.record(message)
            message["telemetry"]["rpm"] = 6100
            message["telemetry"]["throttle"] = 0.6
            recorder.record(message)
            path = recorder.finish()
            self.assertIsNotNone(path)
            data = Path(path).read_bytes()
            metadata_pointer = struct.unpack_from("<I", data, 8)[0]
            data_pointer = struct.unpack_from("<I", data, 12)[0]
            channel_count = struct.unpack_from("<I", data, 86)[0]
            self.assertEqual(metadata_pointer, HEADER_SIZE + EVENT_SIZE)
            self.assertEqual(data_pointer, metadata_pointer + len(CHANNELS) * CHANNEL_HEADER_SIZE)
            self.assertEqual(channel_count, len(CHANNELS))
            self.assertEqual(len(data), data_pointer + len(CHANNELS) * 2 * 4)
            first_count = struct.unpack_from("<I", data, metadata_pointer + 12)[0]
            first_rate = struct.unpack_from("<H", data, metadata_pointer + 22)[0]
            self.assertEqual((first_count, first_rate), (2, 10))
            time_samples = struct.unpack_from("<ff", data, data_pointer)
            throttle_samples = struct.unpack_from("<ff", data, data_pointer + 8)
            self.assertAlmostEqual(time_samples[1], 0.1)
            self.assertAlmostEqual(throttle_samples[0], 50.0)
            self.assertAlmostEqual(throttle_samples[1], 60.0)
            self.assertFalse(any(Path(directory).glob(".rapid-spool-*")))


if __name__ == "__main__":
    unittest.main()
