"""Low-cost Raspberry Pi power-limit monitoring."""
from __future__ import annotations

import re
import subprocess
import threading

from .models import LiveState

# `vcgencmd get_throttled` current flags: undervoltage, frequency cap, throttle.
CURRENT_POWER_LIMIT_MASK = 0x0000_0007
# The same three conditions recorded since boot.
HISTORICAL_POWER_LIMIT_MASK = 0x0007_0000


def parse_throttled(output: str) -> int | None:
    match = re.search(r"0x([0-9a-fA-F]+)", output)
    return int(match.group(1), 16) if match else None


def read_throttled_flags() -> int | None:
    try:
        result = subprocess.run(
            ("vcgencmd", "get_throttled"), capture_output=True, text=True,
            timeout=1, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_throttled(result.stdout) if result.returncode == 0 else None


class PowerLimitMonitor:
    """Poll firmware once per second; never block dashboard/API requests."""

    def __init__(self, state: LiveState):
        self.state = state
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="power-limit-monitor", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)

    def _run(self) -> None:
        while not self._stop.is_set():
            flags = read_throttled_flags()
            self.state.power_status_available = flags is not None
            self.state.throttled_flags = flags
            self.state.power_limited = bool(flags is not None and flags & CURRENT_POWER_LIMIT_MASK)
            self.state.power_limited_since_boot = bool(
                flags is not None and flags & HISTORICAL_POWER_LIMIT_MASK
            )
            self._stop.wait(1)
