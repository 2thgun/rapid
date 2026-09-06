import unittest
from pathlib import Path


class DashboardPageTests(unittest.TestCase):
    def test_dashboard_uses_explicit_dom_references_for_status_updates(self):
        source = (Path(__file__).parents[1] / "rapid" / "web.py").read_text(encoding="utf-8")
        self.assertIn("document.getElementById(id)", source)
        self.assertIn("fields.status.textContent", source)
        self.assertNotIn(";status.textContent", source)
        self.assertIn("id=\"throttle-fill\"", source)
        self.assertIn("id=\"brake-fill\"", source)
        self.assertIn("id=\"steering-wheel\"", source)
        self.assertIn("style.transform = `rotate(", source)
        self.assertIn('class="wheel-logo"', source)
        self.assertIn(">MOMO</text>", source)
        self.assertIn('id="power-status"', source)
        self.assertIn("PWR LIMIT", source)
        self.assertIn("id=\"g-dot\"", source)
        self.assertIn("cursor: none !important", source)


if __name__ == "__main__":
    unittest.main()
