"""Explicit hardware test that stalls the main loop and verifies watchdog reboot."""

import argparse
import time

from scripts.oled_viewer import find_device
from scripts.test_device import status
from scripts.test_support import TestRun, firmware_at_least


def trigger_and_verify_reset(device, report, timeout=22):
    device.timeout = 0.25
    device.write(b"w")
    deadline = time.monotonic() + timeout
    watchdog_triggered = False
    boot_started = False
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        report.detail("SERIAL: " + line)
        lower = line.lower()
        watchdog_triggered |= "task watchdog got triggered" in lower
        boot_started |= line == "RadioRecorder setup starting"
        if boot_started and line == "Watchdog: ready (10 s)":
            if not watchdog_triggered:
                raise AssertionError("Firmware rebooted without a watchdog trigger message")
            return "watchdog panic observed and firmware rebooted"
    raise TimeoutError("Watchdog reboot not confirmed; trigger={}, boot={}".format(watchdog_triggered, boot_started))


def main():
    parser = argparse.ArgumentParser(description="Force a main-loop stall and verify the ESP watchdog reset")
    parser.add_argument("--port", help="ESP serial port (default: auto-detect)")
    args = parser.parse_args()
    report = TestRun("watchdog")
    successful = False
    try:
        connected, device = report.case("ESP serial connection", lambda: find_device(args.port))
        if connected:
            report.detail("Connected to " + device.port)
            with device:
                status_ok, version = report.case("Device prerequisites and watchdog status",
                                                  lambda: status(device, report))
                if status_ok and firmware_at_least(version, (0, 1, 32)):
                    report.case("Watchdog-triggered automatic reset",
                                lambda: trigger_and_verify_reset(device, report))
                elif status_ok:
                    report.skip("Watchdog-triggered automatic reset", "upload firmware 0.1.32 or newer")
        else:
            report.skip("Watchdog-triggered automatic reset", "ESP serial connection failed")
    finally:
        successful = report.summary()
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())