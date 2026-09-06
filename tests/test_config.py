import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from rapid.config import load_settings


class ConfigTests(unittest.TestCase):
    def test_target_defaults_are_safe_and_local_first(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = load_settings(Path(directory) / "missing.toml")
        self.assertFalse(settings.acc_enabled)
        self.assertEqual(settings.app_host, "0.0.0.0")
        self.assertEqual(settings.companion_port, 9001)
        self.assertFalse(settings.upload_enabled)
        self.assertEqual(settings.upload_policy, "races")
        self.assertEqual(settings.upload_retention_days, 14)

    def test_environment_overrides_boolean_and_upload_values(self):
        environment = {
            "RAPID_ACC_ENABLED": "yes",
            "RAPID_COMPANION_KEY": "dGVzdA==",
            "RAPID_UPLOAD_ENABLED": "true",
            "RAPID_UPLOAD_URL": "https://example.invalid/",
            "RAPID_UPLOAD_TOKEN": "secret",
            "RAPID_UPLOAD_POLICY": "manual",
            "RAPID_UPLOAD_RETENTION_DAYS": "30",
        }
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, environment, clear=False):
            settings = load_settings(Path(directory) / "missing.toml")
        self.assertTrue(settings.acc_enabled)
        self.assertTrue(settings.upload_enabled)
        self.assertEqual(settings.upload_url, "https://example.invalid")
        self.assertEqual(settings.upload_policy, "manual")
        self.assertEqual(settings.upload_retention_days, 30)

    def test_invalid_boolean_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(
            os.environ, {"RAPID_UPLOAD_ENABLED": "sometimes"}, clear=False
        ):
            with self.assertRaisesRegex(ValueError, "must be a boolean"):
                load_settings(Path(directory) / "missing.toml")


if __name__ == "__main__":
    unittest.main()
