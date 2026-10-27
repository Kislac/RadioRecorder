"""Shared summary and gitignored log support for PC-driven tests."""

from datetime import datetime
from pathlib import Path
import re
import traceback


class TestRun:
    def __init__(self, suite, log_dir=None):
        safe_suite = re.sub(r"[^a-zA-Z0-9_-]+", "-", suite).strip("-")
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.log_path = Path(log_dir or Path(__file__).resolve().parents[1] / ".test-logs")
        self.log_path.mkdir(parents=True, exist_ok=True)
        self.log_path /= f"{safe_suite}-{timestamp}.log"
        self.results = []
        self._write(f"START {suite}")

    def _write(self, message):
        timestamp = datetime.now().isoformat(timespec="milliseconds")
        with self.log_path.open("a", encoding="utf-8") as output:
            output.write(f"{timestamp} {message}\n")

    def log(self, message, console=True):
        self._write(message)
        if console:
            print(message)

    def detail(self, message):
        self._write(message)

    def case(self, name, action):
        self.log(f"RUN: {name}")
        try:
            result = action()
        except Exception as error:
            self.results.append((name, False))
            self.log(f"FAIL: {name}: {error}")
            self.detail(traceback.format_exc())
            return False, None
        self.results.append((name, True))
        display_result = getattr(result, "port", result)
        self.log(f"PASS: {name}" + (f" - {display_result}" if display_result else ""))
        return True, result

    def skip(self, name, reason):
        self.results.append((name, None))
        self.log(f"SKIP: {name} - {reason}")

    def summary(self):
        passed = sum(result is True for _, result in self.results)
        failed = sum(result is False for _, result in self.results)
        skipped = sum(result is None for _, result in self.results)
        self.log(f"SUMMARY: {passed} passed, {failed} failed, {skipped} skipped")
        self.log(f"LOG: {self.log_path}")
        return failed == 0


def firmware_at_least(version, minimum):
    match = re.match(r"^v?(\d+)\.(\d+)\.(\d+)(?:\.|$)", version)
    return bool(match) and tuple(map(int, match.groups())) >= minimum