"""Offline and online test suites with one final result summary."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

from scripts.test_support import TestRun


def colored(text, color):
    if not sys.stdout.isatty():
        return text
    if os.name == "nt":
        import ctypes

        kernel = ctypes.windll.kernel32
        handle = kernel.GetStdHandle(-11)
        mode = ctypes.c_uint()
        if not kernel.GetConsoleMode(handle, ctypes.byref(mode)) or not kernel.SetConsoleMode(handle, mode.value | 4):
            return text
    return "\033[{}m{}\033[0m".format(color, text)


def run_suite(suite, port=None, base_url="http://192.168.4.1", run=subprocess.run):
    report = TestRun(suite)
    failures = []

    def result(label, passed, detail="", child_output=""):
        report.results.append((label, passed))
        tag = "PASS" if passed is True else "FAIL" if passed is False else "SKIP"
        message = "{}: {}{}".format(tag, label, " - " + detail if detail else "")
        report.log(message, console=False)
        print(colored(message, 32 if passed is True else 31 if passed is False else 33))
        if passed is False:
            issues = re.findall(r"^(?:FAIL|ERROR): .+$", child_output, re.MULTILINE)
            failures.append((label, detail, issues))

    def command(label, args):
        report.log("RUN: " + label, console=False)
        try:
            completed = run(args, cwd=Path(__file__).resolve().parents[1], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        except OSError as error:
            result(label, False, str(error))
            return False
        for line in completed.stdout.splitlines():
            report.detail("OUTPUT: " + line)
        unit_count = re.search(r"Ran (\d+) tests?", completed.stdout)
        child_summary = re.search(r"SUMMARY: (\d+) passed, (\d+) failed, (\d+) skipped", completed.stdout)
        detail = (unit_count.group(1) + " tests" if unit_count else
                  "{} passed, {} failed, {} skipped".format(*child_summary.groups()) if child_summary else
                  "exit {}".format(completed.returncode))
        rtc_skip = re.search(r"^SKIP: RTC (?:without NTP|after software restart) - (.+)$",
                             completed.stdout, re.MULTILINE) if label.startswith("RTC ") else None
        if completed.returncode == 0 and rtc_skip:
            result(label, None, rtc_skip.group(1))
        else:
            result(label, completed.returncode == 0, detail, completed.stdout)
        return completed.returncode == 0

    if suite == "offline":
        command("Offline unit tests", [sys.executable, "-m", "unittest", "discover", "-s", "test",
                                       "-p", "test_*.py", "-v"])
        platformio = Path.home() / ".platformio" / "penv" / "Scripts" / "platformio.exe"
        command("Firmware build", [str(platformio) if platformio.is_file() else shutil.which("pio") or "pio",
                                   "run", "-e", "esp32-s3-devkitc-1"])
    elif suite == "online":
        device = [sys.executable, "-m", "scripts.test_device"]
        if port:
            device.extend(["--port", port])
        if command("Enable diagnostics before online tests", device + ["--ensure-performance"]):
            try:
                command("Device recording and cleanup", device + ["--record", "--language-test", "--auto-test"])
                command("OLED display", device + ["--display-only"])
                command("RTC without NTP and software restart", device + ["--rtc-reboot"])
                try:
                    with urlopen(base_url.rstrip("/") + "/api/status", timeout=2):
                        pass
                except HTTPError:
                    command("Wi-Fi API and recording", [sys.executable, "-m", "scripts.test_wifi",
                                                   "--base-url", base_url, "--record", "--file-manager",
                                                   "--preserve-performance"])
                except (URLError, OSError, TimeoutError) as error:
                    result("Wi-Fi API and recording", None, "ESP HTTP unreachable: " + str(error))
                else:
                    command("Wi-Fi API and recording", [sys.executable, "-m", "scripts.test_wifi",
                                                   "--base-url", base_url, "--record", "--file-manager",
                                                   "--preserve-performance"])
            finally:
                command("Verify diagnostics after online tests", device + ["--check-performance"])
        else:
            result("Device recording and cleanup", None, "diagnostics preflight failed")
            result("OLED display", None, "diagnostics preflight failed")
            result("RTC without NTP and software restart", None, "diagnostics preflight failed")
            result("Wi-Fi API and recording", None, "diagnostics preflight failed")
    else:
        raise ValueError("Unknown test suite: " + suite)

    report.summary()
    if failures:
        print(colored("FAILED CASES:", 31))
        for label, detail, issues in failures:
            print(colored("FAIL: {} - {}".format(label, detail), 31))
            for issue in issues:
                print(colored("  " + issue, 31))
        print("Full output: " + str(report.log_path))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description="Run one consolidated offline or hardware online suite")
    parser.add_argument("suite", choices=("offline", "online"))
    parser.add_argument("--port", help="ESP serial port (online only; default auto-detect)")
    parser.add_argument("--base-url", default="http://192.168.4.1", help="ESP HTTP address (online only)")
    args = parser.parse_args()
    if args.suite != "online" and (args.port or args.base_url != "http://192.168.4.1"):
        parser.error("--port and --base-url are online-only")
    return run_suite(args.suite, args.port, args.base_url)


if __name__ == "__main__":
    raise SystemExit(main())