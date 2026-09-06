import unittest

from rapid.power import (
    CURRENT_POWER_LIMIT_MASK,
    HISTORICAL_POWER_LIMIT_MASK,
    parse_throttled,
)


class PowerLimitTests(unittest.TestCase):
    def test_parses_firmware_flags_and_masks_current_power_limits(self):
        self.assertEqual(parse_throttled("throttled=0x0\n"), 0)
        self.assertEqual(parse_throttled("throttled=0x50005"), 0x50005)
        self.assertIsNone(parse_throttled("not available"))
        self.assertTrue(0x5 & CURRENT_POWER_LIMIT_MASK)
        self.assertFalse(0x8 & CURRENT_POWER_LIMIT_MASK)  # soft temperature limit only
        self.assertTrue(0x50000 & HISTORICAL_POWER_LIMIT_MASK)


if __name__ == "__main__":
    unittest.main()
