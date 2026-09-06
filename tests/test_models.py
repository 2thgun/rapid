import unittest

from rapid.models import LiveState


class LiveStateTests(unittest.TestCase):
    def test_log_activity_advances_only_log_fields(self):
        state = LiveState()
        state.note_log_activity("warn")
        snapshot = state.snapshot()
        self.assertEqual(snapshot["log_sequence"], 1)
        self.assertEqual(snapshot["log_severity"], "warn")
        self.assertIsNotNone(snapshot["log_updated_at"])
        self.assertIsNone(snapshot["received_at"])


if __name__ == "__main__":
    unittest.main()
