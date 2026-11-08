"""Offline and online test suites with one final result summary."""

import argparse
import configparser
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import urlopen

from scripts.test_support import TestRun

DEFAULT_HTTP_URL = "http://192.168.4.1"
ONLINE_CONFIG = Path(__file__).resolve().parents[1] / "online-test.ini"


def online_http_url(mode=None, base_url=None, config_path=ONLINE_CONFIG):
    settings = configparser.ConfigParser(interpolation=None)
    if config_path.is_file():
        settings.read(config_path, encoding="utf-8")
        if not settings.has_section("online"):
            raise ValueError("Online test configuration needs an [online] section")
    options = settings["online"] if settings.has_section("online") else {}
    selected = mode or options.get("mode", "ap")
    if selected not in ("ap", "sta", "forwarded"):
        raise ValueError("Unknown online test mode: " + str(selected))
    url = base_url or options.get(selected, DEFAULT_HTTP_URL if selected == "ap" else None)
    if not isinstance(url, str):
        raise ValueError("Configure the URL for online test mode: " + selected)
    parsed = urlsplit(url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("Online test URL must be an HTTP(S) origin: " + url)
    if parsed.port == 0:
        raise ValueError("Online test URL needs a nonzero port: " + url)
    return url.rstrip("/")


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


def run_suite(suite, port=None, base_url=DEFAULT_HTTP_URL, run=subprocess.run):
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
        report.detail("HTTP target: " + base_url)
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
    parser.add_argument("--mode", choices=("ap", "sta", "forwarded"), help="ESP HTTP route (online only)")
    parser.add_argument("--base-url", help="Override ESP HTTP address for one online run")
    args = parser.parse_args()
    if args.suite != "online" and (args.port or args.mode or args.base_url):
        parser.error("--port, --mode and --base-url are online-only")
    if args.suite == "online":
        try:
            base_url = online_http_url(args.mode, args.base_url)
        except (ValueError, OSError) as error:
            parser.error(str(error))
        print("Online HTTP target: {} ({})".format(base_url, args.mode or "configured mode"))
        return run_suite(args.suite, args.port, base_url)
    return run_suite(args.suite)


if __name__ == "__main__":
    raise SystemExit(main())