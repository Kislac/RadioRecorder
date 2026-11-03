"""Shared summary and gitignored log support for PC-driven tests."""

from datetime import datetime
from pathlib import Path
import re
import traceback


class TestRun:
    def __init__(self, suite, log_dir=None):
        safe_suite = re.sub(r"[^a-zA-Z0-9_-]+", "-", suite).strip("-") or "general"
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.log_path = Path(log_dir or Path(__file__).resolve().parents[1] / ".test-logs") / safe_suite
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

    def case(self, name, action, *, result_formatter=None):
        self.log(f"RUN: {name}")
        try:
            result = action()
        except Exception as error:
            self.results.append((name, False))
            self.log(f"FAIL: {name}: {error}")
            self.detail(traceback.format_exc())
            return False, None
        self.results.append((name, True))
        display_result = result_formatter(result) if result_formatter else getattr(result, "port", result)
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


def validate_performance_snapshot(snapshot):
    if not isinstance(snapshot, dict) or type(snapshot.get("enabled")) is not bool:
        raise AssertionError("Performance enabled state missing")
    if not snapshot["enabled"]:
        if snapshot != {"enabled": False}:
            raise AssertionError("Disabled diagnostics must not collect metrics")
        return "disabled (persistent setting)"
    keys = ("uptimeMs", "loopMaxGapUs", "loopGapsOver100ms", "taskCount",
            "loopStackFreeMinimum", "recorderStackFreeMinimum", "speakerStackFreeMinimum",
            "lastFileWorkerStackFreeMinimum", "adcOverflows", "liveAudioDroppedFrames",
            "internalFree", "internalMinimumFree", "internalLargestBlock",
            "psramFree", "psramMinimumFree", "psramLargestBlock",
            "nvsTotalEntries", "nvsUsedEntries", "nvsFreeEntries")
    for key in keys:
        if type(snapshot.get(key)) is not int or snapshot[key] < 0:
            raise AssertionError("Invalid performance metric: " + key)
    for pool in ("internal", "psram"):
        if snapshot[pool + "LargestBlock"] > snapshot[pool + "Free"]:
            raise AssertionError("Largest block exceeds free " + pool + " heap")
    if snapshot["nvsUsedEntries"] + snapshot["nvsFreeEntries"] > snapshot["nvsTotalEntries"]:
        raise AssertionError("Invalid NVS entry accounting")
    if snapshot["taskCount"] < 3 or not snapshot["loopStackFreeMinimum"]:
        raise AssertionError("Missing running task metrics")
    return "{} tasks; {} NVS entries free; {} B internal heap free".format(
        snapshot["taskCount"], snapshot["nvsFreeEntries"], snapshot["internalFree"])