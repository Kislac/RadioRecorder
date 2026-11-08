import json
import os
import queue
import re
import shutil
import sys
import subprocess
import unittest
from contextlib import redirect_stdout
from itertools import count
from io import StringIO
from pathlib import Path
from urllib.error import URLError
from types import SimpleNamespace
from tempfile import TemporaryDirectory
from unittest.mock import patch
import xml.etree.ElementTree as ET

from scripts.oled_viewer import (FRAME_BYTES, KEY_COMMANDS, SCREENSHOT_DIR, bind_controls, body_pixels,
                                 find_device, parse_frame, pixel, read_frame, save_frame_png)
from scripts.test_device import (DISPLAY_TILES, SETTINGS_TILE, SETTING_COUNT, delete_test_recording, menu_state, rtc_live_check, rtc_snapshot,
                                 rtc_test_eligible, read_until, test_display_all, test_signal, valid_signal, validate_display_frames,
                                 verify_rtc_progress)
from scripts.test_support import TestRun, firmware_at_least
from scripts.test_watchdog import trigger_and_verify_reset
from scripts.run_tests import main, online_http_url, run_suite
from scripts.test_wifi import wifi_profile
from scripts.web_assets import PAGES, render_page


class FakeDevice:
    def __init__(self, *lines):
        self.lines = iter(lines)
        self.commands = []

    def write(self, command):
        self.commands.append(command)

    def readline(self):
        return next(self.lines, b"")

    def close(self):
        self.is_open = False


class FrameTests(unittest.TestCase):
    def test_automatic_playback_is_marked_only_for_radio_sessions(self):
        root = Path(__file__).resolve().parents[1]
        header = (root / "include/playback.h").read_text(encoding="utf-8")
        source = (root / "src/playback.cpp").read_text(encoding="utf-8")
        self.assertIn("bool automatic;", header)
        self.assertIn("automatic && target != PlaybackTarget::radio", source)
        self.assertIn("automaticPlayback.store(automatic);", source)
        self.assertIn("automaticPlayback.store(false);", source)
        self.assertLess(source.index("automaticPlayback.store(false);"),
                source.index("playbackState.store(PlaybackState::idle);", source.index("automaticPlayback.store(false);")))

    def test_automatic_replay_press_stops_even_when_oled_is_asleep(self):
        source = (Path(__file__).resolve().parents[1] / "src/display.cpp").read_text(encoding="utf-8")
        controller = (Path(__file__).resolve().parents[1] / "src/automatic_replay.cpp").read_text(encoding="utf-8")
        poll = source.split("void pollDisplay(int32_t steps, ButtonEvent button) {", 1)[1]
        self.assertLess(poll.index("automaticReplayStop();"), poll.index("if (!ready) return;"))
        self.assertLess(poll.index("automaticReplayStop();"), poll.index("if (screenSleeping) {"))
        self.assertLess(controller.index("replayArmed.store(false);"), controller.index("playbackStop();"))
        self.assertIn("if (!activity && !automaticPlayback.automatic && !automaticReplayArmPending()) return;", poll)
        self.assertIn("if (automaticPlayback.automatic || automaticReplayArmPending()) {", poll)
        self.assertIn("Press = stop", source)
        self.assertIn("automaticPlayback.bytesTotal - automaticPlayback.bytesPlayed", source)

    def test_automatic_replay_restores_only_previous_oled_sleep(self):
        source = (Path(__file__).resolve().parents[1] / "src/display.cpp").read_text(encoding="utf-8")
        poll = source.split("void pollDisplay(int32_t steps, ButtonEvent button) {", 1)[1]
        start = poll.split("if (ready && automaticPlayback.automatic && !automaticPlaybackWasActive) {", 1)[1].split("} else if", 1)[0]
        end = poll.split("} else if (ready && !automaticPlayback.automatic && automaticPlaybackWasActive) {", 1)[1].split("if (automaticPlayback.automatic && button", 1)[0]
        self.assertIn("restoreSleepAfterAutomaticPlayback = screenSleeping;", start)
        self.assertIn("if (screenSleeping) displayWake();", start)
        self.assertIn("if (restoreSleepAfterAutomaticPlayback) {", end)
        self.assertIn("displaySleep();", end)
        self.assertLess(poll.index("if (screenSleeping) displayWake();"), poll.index("automaticReplayStop();"))
        wake = source.split("void displayWake() {", 1)[1].split("void printDisplayState()", 1)[0]
        self.assertIn("if (isSignalTile(tile) && !playbackStatus().automatic) startDisplaySignal();", wake)

    def test_vscode_tasks_keep_utilities_and_only_two_test_suites(self):
        tasks_file = Path(__file__).resolve().parents[1] / ".vscode" / "tasks.json"
        config = json.loads(tasks_file.read_text(encoding="utf-8"))
        tasks = {task["label"]: task for task in config["tasks"]}
        self.assertEqual(len(tasks), 7)
        self.assertEqual(set(tasks), {
            "RadioRecorder: Radio Status (USB, read-only)",
            "RadioRecorder: Radio Read (USB)",
            "RadioRecorder: Radio Write (USB, opt-in)",
            "RadioRecorder: Web Preview (offline)",
            "RadioRecorder: OLED Viewer",
            "RadioRecorder: Offline Tests",
            "RadioRecorder: Online Tests",
        })
        self.assertIn("--confirm-read", tasks["RadioRecorder: Radio Read (USB)"]["args"])
        self.assertIn("--confirm-write", tasks["RadioRecorder: Radio Write (USB, opt-in)"]["args"])
        for label in ("RadioRecorder: Web Preview (offline)", "RadioRecorder: OLED Viewer"):
            self.assertTrue(tasks[label]["isBackground"])
        input_ids = {item["id"] for item in config["inputs"]}
        for task in tasks.values():
            for argument in task["args"]:
                if argument.startswith("${input:"):
                    self.assertIn(argument[8:-1], input_ids)

    def test_test_runner_skips_unreachable_wifi_and_reports_only_failures_at_end(self):
        outputs = []
        with TemporaryDirectory() as folder, patch("scripts.run_tests.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with patch("scripts.run_tests.urlopen", side_effect=URLError("no route")):
                def online_command(command, **kwargs):
                    outputs.append(command)
                    if "--rtc-reboot" in command:
                        return subprocess.CompletedProcess(command, 0, "SKIP: RTC without NTP - upload firmware 0.1.54 or newer\nSUMMARY: 2 passed, 0 failed, 1 skipped\n")
                    return subprocess.CompletedProcess(command, 0, "SUMMARY: 4 passed, 0 failed, 0 skipped\n")

                with redirect_stdout(StringIO()) as console:
                    self.assertEqual(run_suite("online", run=online_command), 0)
                self.assertEqual(len(outputs), 5)
                self.assertIn("--ensure-performance", outputs[0])
                self.assertIn("--record", outputs[1])
                self.assertIn("--rtc-reboot", outputs[3])
                self.assertIn("--check-performance", outputs[4])
                self.assertIn("SKIP: Wi-Fi", console.getvalue())
                self.assertIn("SKIP: RTC without NTP and software restart - upload firmware 0.1.54 or newer", console.getvalue())
                self.assertNotIn("FAILED CASES", console.getvalue())

            outputs.clear()

            def offline_command(command, **kwargs):
                outputs.append(command)
                return subprocess.CompletedProcess(command, 1 if len(outputs) == 1 else 0, "unit failure\n")

            with redirect_stdout(StringIO()) as console:
                self.assertEqual(run_suite("offline", run=offline_command), 1)
            self.assertEqual(len(outputs), 2)
            self.assertEqual(console.getvalue().count("FAILED CASES"), 1)
            self.assertIn("Offline unit tests", console.getvalue().split("FAILED CASES", 1)[1])
            self.assertNotIn("Firmware build", console.getvalue().split("FAILED CASES", 1)[1])

    def test_online_http_url_uses_configured_mode_and_cli_overrides(self):
        with TemporaryDirectory() as folder:
            config = Path(folder) / "online-test.ini"
            self.assertEqual(online_http_url(config_path=config), "http://192.168.4.1")
            config.write_text("; Available modes: ap, sta, forwarded\n[online]\nmode = sta\n"
                              "ap = http://192.168.4.1\nsta = http://192.168.0.117\n"
                              "forwarded = http://203.0.113.10/\n", encoding="utf-8")
            self.assertEqual(online_http_url(config_path=config), "http://192.168.0.117")
            self.assertEqual(online_http_url("forwarded", config_path=config), "http://203.0.113.10")
            self.assertEqual(online_http_url("ap", "http://192.168.4.2:8080/", config), "http://192.168.4.2:8080")
            with self.assertRaises(ValueError):
                online_http_url("forwarded", "http://203.0.113.10/api/status", config)
            with self.assertRaises(ValueError):
                online_http_url("sta", "http://192.168.0.117:invalid", config)
            with self.assertRaises(ValueError):
                online_http_url("sta", "http://192.168.0.117:0", config)
            config.write_text("[other]\nmode = sta\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, r"\[online\] section"):
                online_http_url(config_path=config)

    def test_online_cli_uses_configured_target_without_starting_hardware(self):
        with patch.object(sys, "argv", ["run_tests", "online", "--mode", "sta", "--port", "COM3"]):
            with patch("scripts.run_tests.online_http_url", return_value="http://192.168.0.117") as resolve:
                with patch("scripts.run_tests.run_suite", return_value=0) as suite:
                    with redirect_stdout(StringIO()) as console:
                        self.assertEqual(main(), 0)
        resolve.assert_called_once_with("sta", None)
        suite.assert_called_once_with("online", "COM3", "http://192.168.0.117")
        self.assertIn("http://192.168.0.117", console.getvalue())

    def test_online_suite_routes_selected_http_origin_to_wifi_checks(self):
        commands = []
        target = online_http_url("sta", config_path=Path(__file__).parent / "nonexistent-online-config.json",
                                 base_url="http://192.168.0.117")

        def fake_command(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0, "SUMMARY: 1 passed, 0 failed, 0 skipped\n")

        with TemporaryDirectory() as folder, patch("scripts.run_tests.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with patch("scripts.run_tests.urlopen") as probe:
                self.assertEqual(run_suite("online", base_url=target, run=fake_command), 0)
            report = next(Path(folder).glob("online/*.log")).read_text(encoding="utf-8")
        self.assertEqual(probe.call_args.args[0], "http://192.168.0.117/api/status")
        self.assertIn("--base-url", commands[4])
        self.assertEqual(commands[4][commands[4].index("--base-url") + 1], target)
        self.assertIn("HTTP target: " + target, report)

    def test_test_runner_reports_reachable_broken_wifi_api_as_failure(self):
        outputs = []

        def command_result(command, **kwargs):
            outputs.append(command)
            if "scripts.test_wifi" in command:
                return subprocess.CompletedProcess(command, 1, "FAIL: Wi-Fi pages and read-only APIs: HTTP 500\nSUMMARY: 0 passed, 1 failed, 0 skipped\n")
            return subprocess.CompletedProcess(command, 0, "SUMMARY: 3 passed, 0 failed, 0 skipped\n")

        with TemporaryDirectory() as folder, patch("scripts.run_tests.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with patch("scripts.run_tests.urlopen"):
                with redirect_stdout(StringIO()) as console:
                    self.assertEqual(run_suite("online", run=command_result), 1)
        self.assertEqual(len(outputs), 6)
        self.assertIn("--ensure-performance", outputs[0])
        self.assertIn("--record", outputs[4])
        self.assertIn("--preserve-performance", outputs[4])
        self.assertIn("--check-performance", outputs[5])
        self.assertIn("FAIL: Wi-Fi API and recording", console.getvalue().split("FAILED CASES", 1)[1])
        self.assertIn("HTTP 500", console.getvalue().split("FAILED CASES", 1)[1])
        self.assertNotIn("FAIL: Device recording and cleanup", console.getvalue().split("FAILED CASES", 1)[1])

    def test_online_suite_aborts_mutations_when_diagnostics_preflight_fails(self):
        outputs = []

        def preflight_fails(command, **kwargs):
            outputs.append(command)
            return subprocess.CompletedProcess(command, 1, "FAIL: Enable diagnostics before tests: NVS write failed\n")

        with TemporaryDirectory() as folder, patch("scripts.run_tests.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with redirect_stdout(StringIO()) as console:
                self.assertEqual(run_suite("online", run=preflight_fails), 1)
        self.assertEqual(len(outputs), 1)
        self.assertIn("--ensure-performance", outputs[0])
        self.assertIn("SKIP: Device recording and cleanup", console.getvalue())

    def test_online_suite_checks_diagnostics_after_device_failure(self):
        outputs = []

        def device_fails(command, **kwargs):
            outputs.append(command)
            if "--record" in command:
                return subprocess.CompletedProcess(command, 1, "FAIL: Device test failed\n")
            return subprocess.CompletedProcess(command, 0, "SUMMARY: 1 passed, 0 failed, 0 skipped\n")

        with TemporaryDirectory() as folder, patch("scripts.run_tests.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with patch("scripts.run_tests.urlopen", side_effect=URLError("no route")):
                self.assertEqual(run_suite("online", run=device_fails), 1)
        self.assertIn("--check-performance", outputs[-1])

    def test_rtc_progress_requires_offline_clock_and_elapsed_time(self):
        before = {"rtc": 1793270000, "system": 1793270000, "source": "R", "offline": True}
        after = {"rtc": 1793270003, "system": 1793270003, "source": "R", "offline": True}
        self.assertIn("3s", verify_rtc_progress(before, after, 3.2, reboot=True))
        self.assertTrue(rtc_test_eligible(before))
        self.assertFalse(rtc_test_eligible({**before, "offline": False}))
        self.assertFalse(rtc_test_eligible({**before, "source": "N"}))
        for invalid in ({**after, "offline": False}, {**after, "source": "N"},
                        {**after, "rtc": before["rtc"]}, {**after, "system": after["rtc"] + 20}):
            with self.subTest(invalid=invalid), self.assertRaises(AssertionError):
                verify_rtc_progress(before, invalid, 3.2, reboot=True)

    def test_rtc_serial_probe_rejects_ntp_and_refused_reboot(self):
        class Report:
            def detail(self, message):
                pass

        report = Report()
        online = FakeDevice(b"RTC_TIME rtc=1793270000 system=1793270000 source=N offline=0\n")
        with self.assertRaisesRegex(AssertionError, "NTP-free"):
            rtc_live_check(online, report, reboot=True)
        self.assertEqual(online.commands, [b"q"])

        refused = FakeDevice(b"RTC_TIME rtc=1793270000 system=1793270000 source=R offline=1\n",
                             b"RTC_RESTART refused: busy\n")
        with self.assertRaisesRegex(AssertionError, "refused"):
            rtc_live_check(refused, report, reboot=True)
        self.assertEqual(refused.commands, [b"q", b"y"])
        self.assertEqual(rtc_snapshot(FakeDevice(b"RTC_TIME rtc=1793270002 system=1793270002 source=R offline=1\n"),
                                      report)["rtc"], 1793270002)

        recovered = FakeDevice(b"RTC_TIME rtc=1793270000 system=1793270000 source=R offline=1\n",
                       b"RTC_RESTART accepted\n", b"RadioRecorder setup starting\n",
                       b"Wi-Fi mode: AP\n",
                       b"RTC_TIME rtc=1793270001 system=1793270001 source=R offline=1\n")
        self.assertIn("after software reboot", rtc_live_check(recovered, report, reboot=True))
        self.assertEqual(recovered.commands, [b"q", b"y", b"q"])

    def test_oled_rms_max_rescales_history_and_resets_on_entry(self):
        source = (Path(__file__).resolve().parents[1] / "src" / "display.cpp").read_text(encoding="utf-8")
        start = source.split("void startDisplaySignal()", 1)[1].split("void stopDisplaySignal()", 1)[0]
        sample = source.split("void sampleDisplaySignal(uint32_t now)", 1)[1].split("void drawSignal()", 1)[0]
        graph = source.split("void drawSignal()", 1)[1].split("void drawRecordingName", 1)[0]
        self.assertIn("graphMaxRms = 0", start)
        self.assertIn("signalActiveAt = 0", start)
        self.assertIn("kSignalSettlingMs = 2000", source)
        self.assertIn("if (!signal.enabled || !signal.active) { signalActiveAt = 0; return; }", sample)
        self.assertIn("if (!signalActiveAt) { signalActiveAt = now; lastGraphSampleAt = now; }", sample)
        settling = "if (static_cast<uint32_t>(now - signalActiveAt) < kSignalSettlingMs) return;"
        self.assertIn(settling, sample)
        self.assertLess(sample.index(settling), sample.index("if (signal.rms > graphMaxRms)"))
        self.assertLess(sample.index(settling), sample.index("graphSamples[graphNext] = signal.rms"))
        self.assertIn("if (signal.rms > graphMaxRms) graphMaxRms = signal.rms", sample)
        self.assertIn("graphSamples[graphNext] = signal.rms", sample)
        self.assertIn("graphNext = (graphNext + 1) % kGraphSamples", sample)
        self.assertIn("graphCount < kGraphSamples", sample)
        self.assertIn("sample) * 24 / graphMaxRms", graph)
        self.assertIn('"Max RMS %u"', graph)

    def test_speaker_settings_are_debounced_and_restored(self):
        root = Path(__file__).resolve().parents[1]
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        setters = playback.split("void playbackSetVolume", 1)[1].split("void playbackPollSettings", 1)[0]
        poll = playback.split("void playbackPollSettings", 1)[1].split("static void speakerLoop", 1)[0]
        initialization = playback.split("void initPlayback(bool sdReady)", 1)[1]
        self.assertIn("speakerSettingsChangedAt = millis()", setters)
        self.assertNotIn("putUChar", setters)
        self.assertIn("kSpeakerSaveDelayMs = 1000", playback)
        self.assertIn("millis() - speakerSettingsChangedAt) < kSpeakerSaveDelayMs", poll)
        self.assertIn("volume == savedSpeakerVolume && boost == savedSpeakerBoost", poll)
        self.assertIn("putUChar(kSpeakerVolumeKey, volume)", poll)
        self.assertIn("putUChar(kSpeakerBoostKey, boost)", poll)
        self.assertIn("getUChar(kSpeakerVolumeKey", initialization)
        self.assertIn("getUChar(kSpeakerBoostKey", initialization)
        self.assertIn("playbackPollSettings();", main)

    def test_oled_menu_redesign_contract(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        self.assertIn("kTileCount = 12", source)
        self.assertIn('"Manual radio record", "Manual mic record", "Auto radio record"', source)
        self.assertIn('"File manager", "Radio signal", "Microphone signal"', source)
        self.assertIn("kBrowseRows = 5", source)
        self.assertIn("if (!inMenu) oled.drawHLine(0, 56, 128);", source)
        self.assertIn('snprintf(footer, sizeof(footer), "< %u/%u >"', source)
        self.assertNotIn("Hosszu: vissza", source)
        self.assertIn("drawRecordingName(recorderName())", source)
        self.assertIn("if (isSignalTile(previousTile)) stopDisplaySignal();", source)
        self.assertIn("if (isSignalTile(tile)) startDisplaySignal();", source)
        self.assertIn("recorderSetMicSignal(true)", source)
        self.assertIn("recorderSetMicSignal(false)", source)
        self.assertIn("sampleDisplaySignal(now)", source)
        self.assertIn("graphSamples[120]", source)
        self.assertIn("if (pttTestActive()) stopPttTest();", source)
        self.assertIn("else if (!latchPttTest())", source)
        self.assertIn("WiFi.localIP().toString()", source)
        self.assertIn("WiFi.softAPIP().toString()", source)
        self.assertIn('"%04d.%02u.%02d %s"', source)

    def test_compact_header_and_time_tile_use_local_clock_and_monotonic_runtime(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        header = source.split("void drawStatus()", 1)[1].split("void drawTime()", 1)[0]
        time_tile = source.split("void drawTime()", 1)[1].split("void drawSettingValue", 1)[0]
        self.assertIn('"%y%m%d %H:%M:%S"', header)
        self.assertIn("128 - oled.getStrWidth(clockText)", header)
        self.assertNotIn("appClockIndicator", header)
        self.assertGreaterEqual(128 - len("261101 16:10:02") * 5, 26 + len("REC") * 5 + 2)
        self.assertIn('"%Y.%m.%d %H:%M:%S"', time_tile)
        self.assertLessEqual(len("2026.11.01 16:21:28") * 5, 128)
        self.assertIn("appClockSourceLabel()", time_tile)
        self.assertIn("appUptimeText()", time_tile)
        self.assertIn("kTimeTile = 10", source)
        self.assertIn("kSettingsTile = kTileCount - 1", source)
        self.assertIn('"Time", "Settings"', source)
        names = re.findall(r'"([^"]+)"', source.split("kTileNames[kTileCount] = {", 1)[1].split("};", 1)[0])
        self.assertEqual(tuple(names), DISPLAY_TILES)
        self.assertEqual((len(DISPLAY_TILES), SETTINGS_TILE, DISPLAY_TILES[-1]), (12, 11, "Settings"))
        self.assertIn("tile == kWifiTile || tile == kTimeTile", source)
        uptime = control.split("uint64_t appUptimeSeconds()", 1)[1].split("static String clockTimezoneRule()", 1)[0]
        self.assertIn("esp_timer_get_time()", uptime)
        self.assertNotIn("millis()", uptime)
        self.assertNotIn("time(nullptr)", uptime)
        self.assertIn("days / 360ULL", uptime)
        self.assertIn("days % 30ULL", uptime)

    def test_firmware_runtime_formatter_survives_millis_wrap_and_unit_rollovers(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for firmware runtime regression")
        source = (Path(__file__).resolve().parents[1] / "src" / "app_control.cpp").read_text(encoding="utf-8")
        functions = "uint64_t appUptimeSeconds()" + source.split("uint64_t appUptimeSeconds()", 1)[1].split("static String clockTimezoneRule()", 1)[0]
        harness = """
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
using String = std::string;
int64_t timerMicros = 0;
int64_t esp_timer_get_time() { return timerMicros; }
""" + functions + """
int main() {
  const uint64_t samples[] = {0, 59, 60, 86399, 86400, 30ULL*86400, 360ULL*86400, 4294968};
  const char *expected[] = {"00:00:00 00:00:00", "00:00:00 00:00:59", "00:00:00 00:01:00",
                           "00:00:00 23:59:59", "00:00:01 00:00:00", "00:01:00 00:00:00",
                           "01:00:00 00:00:00", "00:01:19 17:02:48"};
  for (size_t index = 0; index < sizeof(samples)/sizeof(samples[0]); ++index) {
    timerMicros = static_cast<int64_t>(samples[index]*1000000ULL + 999999ULL);
    assert(appUptimeSeconds() == samples[index]);
    assert(appUptimeText() == expected[index]);
  }
}
"""
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "runtime.cpp").write_text(harness, encoding="utf-8")
            executable = path / ("runtime.exe" if os.name == "nt" else "runtime")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                                       str(path / "runtime.cpp"), "-o", str(executable)],
                                      capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_display_probe_keeps_settings_last_after_time_tile(self):
        state = {"tile": 0, "setting": 0, "settings": False, "browser": False}
        visited = []

        def write(command):
            if command == b"l":
                field = "setting" if state["settings"] else "tile"
                state[field] = (state[field] + 1) % (SETTING_COUNT if state["settings"] else 12)
            elif command == b"k":
                self.assertIn(state["tile"], (4, SETTINGS_TILE))
                state["settings"] = state["tile"] == SETTINGS_TILE
                state["browser"] = state["tile"] == 4
            elif command == b"b":
                state["settings"] = state["browser"] = False

        def frame(device):
            visited.append((state["tile"], state["settings"], state["browser"]))
            pixels = bytearray(FRAME_BYTES)
            position = state["setting"] + 32 if state["settings"] else state["tile"]
            pixels[256 + position] = 2 if state["browser"] else 1
            return bytes(pixels)

        device = SimpleNamespace(write=write)
        report = SimpleNamespace(detail=lambda message: None)
        with patch("scripts.test_device.menu_state", side_effect=lambda device:
                   (state["tile"], state["setting"], int(state["settings"]), 0, 0)), \
                patch("scripts.test_device.read_frame", side_effect=frame):
            self.assertIn("all tiles/settings rendered", test_display_all(device, report))
        self.assertIn((10, False, False), visited)
        self.assertIn((SETTINGS_TILE, True, False), visited)
        self.assertEqual((state["tile"], state["settings"], state["browser"]), (0, False, False))

    def test_oled_sd_browser_and_radio_dac_playback(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        browser = source.split("void refreshBrowseRows()", 1)[1].split("String shortBrowseName", 1)[0]
        playback = source.split("void pressBrowse()", 1)[1].split("void drawBrowse()", 1)[0]
        self.assertIn("fileManagerListOffset", browser)
        manager = (root / "src" / "file_manager.cpp").read_text(encoding="utf-8")
        self.assertIn('fileManagerList(directory, "date", true', manager)
        self.assertIn('lower.endsWith(".wav")', manager)
        self.assertIn('browseRows[0] = {".."', browser)
        self.assertIn("if (playbackChoice == 2) { confirmingDelete = true; playbackChoice = 0;", playback)
        self.assertIn("fileManagerDeleteFile(selectedFile, error)", playback)
        self.assertIn("if (playbackChoice == 3)", playback)
        self.assertIn("PlaybackTarget target = playbackChoice == 1 ? PlaybackTarget::radio", playback)
        self.assertIn("appCanStartPlayback()", playback)
        self.assertIn("findPcmData(file, remaining)", playback)
        self.assertIn("playbackStart(selectedFile)", playback)
        self.assertIn("playbackStart(selectedFile, PlaybackTarget::radio)", playback)
        self.assertNotIn("startPttTest", playback)

    def test_wifi_boot_connect_fallback_and_keep_ap_contract(self):
        root = Path(__file__).resolve().parents[1]
        wifi_source = (root / "src" / "wifi_manager.cpp").read_text(encoding="utf-8")
        page_source = " ".join("\n".join(render_page(name) for name in PAGES).split())
        server_source = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        main_source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('kDefaultTimeoutMinutes = 5', wifi_source)
        self.assertIn('Serial.printf("Wi-Fi auto-off: %u min\\n", wifiManagerAutoOffMinutes())', main_source)
        self.assertIn('kConnectTimeoutMs = 15000', wifi_source)
        self.assertIn('preferences.getBool(kKeepApKey, false)', wifi_source)
        self.assertIn('if (savedSsid.isEmpty())', wifi_source)
        self.assertIn('startAp(false)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_AP_STA)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_STA)', wifi_source)
        self.assertIn('fallbackToAp("connection timed out after 15 seconds")', wifi_source)
        self.assertIn('passwordBytes == password.length()', wifi_source)
        self.assertNotIn('cJSON_AddStringToObject(root, "password"', wifi_source)
        self.assertIn('server.on("/api/wifi/scan", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/connect", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/forget", HTTP_POST', server_source)
        self.assertIn('id="keepApHost" type="checkbox"', page_source)
        self.assertNotIn('id="keepApHost" type="checkbox" checked', page_source)

    def test_watchdog_monitors_idle_cores_and_feeds_active_tasks(self):
        root = Path(__file__).resolve().parents[1]
        watchdog = (root / "src" / "watchdog.cpp").read_text(encoding="utf-8")
        main_source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        recorder_source = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        playback_source = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        main_source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        watchdog_test_source = (root / "scripts" / "test_watchdog.py").read_text(encoding="utf-8")
        self.assertIn("kWatchdogTimeoutMs = 10000", watchdog)
        self.assertIn("kWatchdogIdleCoreMask = (1U << portNUM_PROCESSORS) - 1", watchdog)
        self.assertIn("config.trigger_panic", watchdog)
        self.assertIn("esp_task_wdt_reconfigure(&config)", watchdog)
        self.assertIn("initSystemWatchdog()", main_source)
        self.assertIn("feedSystemWatchdog()", main_source)
        self.assertIn('Serial.println("Watchdog: active (10 s, both cores)")', main_source)
        self.assertIn("watchdogResetCurrentTask()", recorder_source)
        self.assertIn("watchdogResetCurrentTask()", playback_source)
        self.assertIn("if (command == 'w')", main_source)
        self.assertIn("if (command == 'W')", main_source)
        self.assertIn("kWatchdogConfirmMs = 5000", main_source)
        self.assertIn("recorderState() == CaptureState::idle", main_source)
        self.assertIn("radioProgrammingBusy()", main_source)
        self.assertIn('device.write(b"w")', watchdog_test_source)
        self.assertIn('device.write(b"W")', watchdog_test_source)
        self.assertIn("task watchdog got triggered", watchdog_test_source)
        self.assertIn('line == "Watchdog: ready (10 s)"', watchdog_test_source)

    def test_watchdog_tester_does_not_confirm_refused_request(self):
        device = FakeDevice(b"Watchdog test refused: device busy\n")
        report = SimpleNamespace(detail=lambda line: None)
        with patch("scripts.test_watchdog.time.monotonic", side_effect=[0, 0, 4]):
            with self.assertRaisesRegex(TimeoutError, "confirmation prompt not received"):
                trigger_and_verify_reset(device, report)
        self.assertEqual(device.commands, [b"w"])

    def test_persistent_screen_timeout_web_settings_contract(self):
        root = Path(__file__).resolve().parents[1]
        page_source = " ".join("\n".join(render_page(name) for name in PAGES).split())
        server_source = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        app_source = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        display_source = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        playback_source = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        recorder_source = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        startup_image = (root / "include" / "startup_image.h").read_text(encoding="utf-8")
        wifi_source = (root / "src" / "wifi_manager.cpp").read_text(encoding="utf-8")
        wifi_source = (root / "src" / "wifi_manager.cpp").read_text(encoding="utf-8")
        self.assertIn('href="/settings"', page_source)
        self.assertIn('href="/"', page_source)
        self.assertIn('type="number" min="0" max="999" step="1"', page_source)
        self.assertIn('server.on("/api/settings", HTTP_GET, sendSettings)', server_source)
        self.assertIn('server.on("/api/settings", HTTP_POST, saveSettings)', server_source)
        self.assertIn('if (value.isEmpty() || value.length() > 3)', server_source)
        self.assertIn('auto parseMinutes = [](const String &value, uint16_t &minutes)', server_source)
        self.assertIn('return minutes <= 999;', server_source)
        self.assertIn('server.on("/api/display/wake", HTTP_POST', server_source)
        self.assertIn('server.send(503, "text/plain", "OLED unavailable")', server_source)
        self.assertIn('kDefaultScreenOffMinutes = 5', app_source)
        self.assertIn('preferences.getUShort(kScreenOffKey, kDefaultScreenOffMinutes)', app_source)
        self.assertIn('preferences.putUShort(kScreenOffKey, minutes)', app_source)
        self.assertIn('kMaxScreenOffMinutes = 999', app_source)
        self.assertIn('oled.setPowerSave(0)', display_source)
        self.assertIn('oled.setPowerSave(1)', display_source)
        self.assertIn('snprintf(value, sizeof(value), "0 min")', display_source)
        wake_branch = display_source.split("if (screenSleeping)", 1)[1].split("bool redraw", 1)[0]
        self.assertIn("drawBody();", wake_branch)
        self.assertIn("return;", wake_branch)
        self.assertIn('Serial.printf("OLED_STATE %u %u %u %u %u\\n", tile, setting, inSettings, editing, screenSleeping)', display_source)
        self.assertIn("if (command == 'o') displaySleep();", (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn("Wake display", page_source)
        self.assertIn("/api/display/wake", page_source)
        self.assertIn("oled.drawXBM(0, 0, 128, 64, kStartupImageBits)", display_source)
        self.assertIn("playbackPlayCue(PlaybackCue::startup)", (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn("playbackRecordCue(automaticFile.load(), microphone, true)", recorder_source)
        self.assertIn("if (valid) playbackRecordCue(automaticFile.load(), microphone, false)", recorder_source)
        self.assertIn('static_assert(sizeof(kStartupImageBits) == 1024', startup_image)
        self.assertIn("sinf(phase)", playback_source)
        self.assertIn("if (speakerReady &&", playback_source)
        self.assertIn("cuePlaying.compare_exchange_strong", playback_source)
        self.assertIn("cuePlaying.load() ? PlaybackState::playing", playback_source)
        self.assertIn('playbackPlayCue(PlaybackCue::startup)', (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn('String(__DATE__) + " " + String(__TIME__)', app_source)
        self.assertIn('Serial.printf("Build time: %s\\n"', (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn('drawBuildDate();', display_source)
        self.assertIn('kWifiTimeoutKey[] = "wifiOffMin"', wifi_source)
        self.assertIn('kDefaultTimeoutMinutes = 5', wifi_source)
        self.assertIn('kConnectTimeoutMs = 15000', wifi_source)
        self.assertIn('preferences.getBool(kKeepApKey, false)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_AP_STA)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_STA)', wifi_source)
        self.assertIn('fallbackToAp("connection timed out after 15 seconds")', wifi_source)
        self.assertNotIn('cJSON_AddStringToObject(root, "password"', wifi_source)
        self.assertIn('server.on("/api/wifi/scan", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/connect", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/forget", HTTP_POST', server_source)
        self.assertIn('id="keepApHost" type="checkbox"', page_source)
        self.assertNotIn('id="keepApHost" type="checkbox" checked', page_source)
        self.assertIn('"Wi-Fi", "Time", "Settings"', display_source)
        self.assertIn('kWifiTimeoutKey[] = "wifiOffMin"', wifi_source)
        self.assertIn('kDefaultTimeoutMinutes = 5', wifi_source)
        self.assertIn('kConnectTimeoutMs = 15000', wifi_source)
        self.assertIn('preferences.getBool(kKeepApKey, false)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_AP_STA)', wifi_source)
        self.assertIn('WiFi.mode(WIFI_STA)', wifi_source)
        self.assertIn('fallbackToAp("connection timed out after 15 seconds")', wifi_source)
        self.assertNotIn('cJSON_AddStringToObject(root, "password"', wifi_source)
        self.assertIn('server.on("/api/wifi/scan", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/connect", HTTP_POST', server_source)
        self.assertIn('server.on("/api/wifi/forget", HTTP_POST', server_source)
        self.assertIn('id="keepApHost" type="checkbox"', page_source)
        self.assertNotIn('id="keepApHost" type="checkbox" checked', page_source)

    def test_display_frame_validation_requires_distinct_visible_views(self):
        frames = []
        for x in range(7):
            frame = bytearray(FRAME_BYTES)
            frame[2 * 128 + x] = 1
            frames.append(bytes(frame))
        self.assertIn("7 distinct", validate_display_frames(frames, 7, "main tiles"))
        with self.assertRaisesRegex(AssertionError, "duplicate"):
            validate_display_frames(frames[:1] * 2, 2, "settings rows")
        with self.assertRaisesRegex(AssertionError, "blank"):
            validate_display_frames([bytes(FRAME_BYTES)], 1, "main tiles")

    def test_firmware_version_requirement_accepts_optional_v_prefix(self):
        self.assertTrue(firmware_at_least("v0.1.26.23", (0, 1, 26)))
        self.assertTrue(firmware_at_least("0.1.27.1", (0, 1, 26)))
        self.assertFalse(firmware_at_least("v0.1.25.99", (0, 1, 26)))
        self.assertFalse(firmware_at_least("unknown", (0, 1, 26)))

    def test_test_recording_delete_uses_exact_filename(self):
        filename = "/2026-10-28/RadioMan261028_232530.wav"
        device = FakeDevice(b"Delete test WAV: send exact recording path\n",
                            b"Test WAV deleted: /2026-10-28/RadioMan261028_232530.wav\n")
        with patch("scripts.test_device.read_until", wraps=read_until) as reader:
            self.assertEqual(delete_test_recording(device, filename), "removed " + filename)
        self.assertEqual(reader.call_args.kwargs["timeout"], 8)
        self.assertEqual(device.commands, [b"x", filename.encode() + b"\n"])

    def test_test_recording_delete_rejects_unexpected_path(self):
        device = FakeDevice()
        with self.assertRaises(ValueError):
            delete_test_recording(device, "../REC00042.wav")
        with self.assertRaises(ValueError):
            delete_test_recording(device, "/2026-10-28/MicMan261028_232530.wav")
        self.assertEqual(device.commands, [])

    def test_test_run_summary_is_saved_to_log(self):
        with TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            run = TestRun("offline test", log_dir=directory)
            self.assertEqual(run.log_path.parent, Path(directory) / "offline-test")
            self.assertTrue(run.case("frame check", lambda: "valid")[0])
            self.assertTrue(run.case("serial connection", lambda: SimpleNamespace(port="COM3"))[0])
            run.skip("recording", "opt-in")
            self.assertTrue(run.summary())
            log = run.log_path.read_text(encoding="utf-8")
        self.assertIn("PASS: frame check - valid", log)
        self.assertIn("PASS: serial connection - COM3", log)
        self.assertIn("SUMMARY: 2 passed, 0 failed, 1 skipped", log)

    def test_test_runs_keep_default_and_custom_log_roots_free_of_files(self):
        suites = ("offline", "online", "device", "display", "rtc", "rtc-reboot",
                  "wifi", "watchdog", "radio-programmer", "../unsafe suite", "...")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            with patch("scripts.test_support.__file__", str(root / "scripts" / "test_support.py")):
                for log_dir in (None, root / "custom-logs"):
                    log_root = root / ".test-logs" if log_dir is None else log_dir
                    for suite in suites:
                        with self.subTest(suite=suite, log_dir=log_dir):
                            run = TestRun(suite, log_dir=log_dir)
                            self.assertEqual(run.log_path.parent.parent, log_root)
                            self.assertTrue(run.log_path.is_file())
                            self.assertTrue(run.log_path.name.startswith(run.log_path.parent.name + "-"))
                    self.assertTrue(all(entry.is_dir() for entry in log_root.iterdir()))

    def test_test_run_summary_marks_failures(self):
        with TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            run = TestRun("offline test", log_dir=directory)
            self.assertFalse(run.case("broken check", lambda: 1 / 0)[0])
            self.assertFalse(run.summary())
            log = run.log_path.read_text(encoding="utf-8")
        self.assertIn("FAIL: broken check", log)
        self.assertIn("SUMMARY: 0 passed, 1 failed, 0 skipped", log)

    def test_signal_sample_must_be_in_adc_range(self):
        self.assertEqual(valid_signal(["GPIO1 RMS: 12 | Peak: 30 | DC-offset: 2010 (ADC)"]), (12, 30, 2010))
        for line in ("GPIO1 RMS: 31 | Peak: 30 | DC-offset: 2010 (ADC)",
                 "GPIO1 RMS: 12 | Peak: 30 | DC-offset: 5000 (ADC)",
                 "ADC measurement error (3)"):
            with self.subTest(line=line), self.assertRaises(AssertionError):
                valid_signal([line])

    def test_serial_signal_probe_accepts_english_meter_responses(self):
        device = FakeDevice(b"Serial ADC measurement enabled\n",
                            b"GPIO1 RMS: 12 | Peak: 30 | DC-offset: 2010 (ADC)\n",
                            b"Serial ADC measurement disabled\n")
        states = [(SETTINGS_TILE, 0, 0, 0, 0), (SETTINGS_TILE, 0, 1, 0, 0),
                  (SETTINGS_TILE, 1, 1, 0, 0), (SETTINGS_TILE, 1, 1, 0, 0),
                  (SETTINGS_TILE, 1, 0, 0, 0)]
        report = SimpleNamespace(detail=lambda line: None)
        with patch("scripts.test_device.menu_state", side_effect=states):
            self.assertEqual(test_signal(device, report), "GPIO1 RMS=12 peak=30 bias=2010")
        self.assertEqual(device.commands, [b"k", b"l", b"k", b"k", b"b"])

    def test_auto_detect_skips_unresponsive_port(self):
        other = FakeDevice(b"Other device\n")
        other.is_open = True
        esp = FakeDevice(b"Firmware: v0.1.24\n", b"Recorder: ready\n", b"Wi-Fi IP: 0.0.0.0\n")
        esp.port = "COM8"
        ticks = count()
        with patch("serial.tools.list_ports.comports", return_value=[type("Port", (), {"device": "COM7"})(),
                                          type("Port", (), {"device": "COM8"})()]), \
             patch("scripts.oled_viewer.open_device", side_effect=[other, esp]), \
             patch("scripts.oled_viewer.time.monotonic", side_effect=lambda: next(ticks) / 10):
            self.assertIs(find_device(), esp)
        self.assertFalse(other.is_open)
        self.assertEqual(other.commands, [b"i"])
        self.assertEqual(esp.commands, [b"i"])

    def test_space_sends_long_press(self):
        commands = dict(KEY_COMMANDS)
        self.assertEqual(commands["<Left>"], "j")
        self.assertEqual(commands["<Up>"], "j")
        self.assertEqual(commands["<Right>"], "l")
        self.assertEqual(commands["<Down>"], "l")
        self.assertEqual(commands["<Return>"], "k")
        self.assertEqual(commands["<space>"], "b")
        self.assertEqual(commands["<Escape>"], "b")

    def test_keyboard_controls_do_not_activate_focused_buttons(self):
        import tkinter as tk

        root = tk.Tk()
        self.addCleanup(root.destroy)
        commands = queue.Queue()
        activated = []
        toolbar = tk.Frame(root)
        toolbar.pack()
        button = tk.Button(toolbar, text="Save PNG", command=lambda: activated.append(True))
        button.pack()
        bind_controls(root, commands)
        root.update()
        button.focus_force()
        root.update()
        for key, expected in KEY_COMMANDS:
            with self.subTest(key=key):
                button.event_generate(key)
                button.event_generate(key.replace("<", "<KeyRelease-"))
                root.update()
                self.assertEqual(commands.get_nowait(), expected)
                self.assertTrue(commands.empty())
                self.assertEqual(activated, [])

    def test_saves_timestamped_png_with_correct_oled_pixels(self):
        import tkinter as tk

        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        frame = bytearray(FRAME_BYTES)
        frame[0] = 1
        frame[128] = 128
        frame[-1] = 128
        self.assertEqual(SCREENSHOT_DIR, Path(__file__).resolve().parents[1] / ".test-logs" / "oled")
        with TemporaryDirectory() as folder:
            directory = Path(folder) / "logs" / "oled"
            first = save_frame_png(frame, root, directory)
            second = save_frame_png(bytes(FRAME_BYTES), root, directory)
            self.assertEqual(first.parent, directory)
            self.assertRegex(first.name, r"^oled-\d{8}-\d{6}-\d{6}\.png$")
            self.assertNotEqual(first, second)
            self.assertEqual(first.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
            image = tk.PhotoImage(master=root, file=str(first))
            self.assertEqual((image.width(), image.height()), (128, 64))
            for coordinates in ((0, 0), (0, 15), (127, 63)):
                self.assertEqual(image.get(*coordinates), (198, 241, 115))
            self.assertEqual(image.get(0, 1), (20, 26, 25))
            blank = tk.PhotoImage(master=root, file=str(second))
            self.assertEqual(blank.get(0, 0), (20, 26, 25))

    def test_decodes_u8g2_vertical_tiles(self):
        frame = bytearray(FRAME_BYTES)
        frame[0] = 1
        frame[128] = 128
        frame[-1] = 128
        decoded = parse_frame(b"OLED_FRAME 128 64 " + frame.hex().encode() + b"\n")
        self.assertTrue(pixel(decoded, 0, 0))
        self.assertTrue(pixel(decoded, 0, 15))
        self.assertTrue(pixel(decoded, 127, 63))
        self.assertFalse(pixel(decoded, 0, 1))

    def test_rejects_truncated_and_invalid_frames(self):
        for line in (b"OLED_FRAME 128 64 00\n", b"OLED_FRAME 128 64 " + b"Z" * 2048):
            with self.subTest(line=line[:40]), self.assertRaises(ValueError):
                parse_frame(line)

    def test_body_ignores_status_and_footer(self):
        frame = bytearray(FRAME_BYTES)
        frame[0] = 1
        frame[7 * 128] = 1
        self.assertEqual(body_pixels(frame), body_pixels(bytes(FRAME_BYTES)))
        frame[2 * 128] = 1
        self.assertNotEqual(body_pixels(frame), body_pixels(bytes(FRAME_BYTES)))

    def test_reads_real_frame_after_other_serial_output(self):
        frame = bytes([0xA5]) * FRAME_BYTES
        device = FakeDevice(b"Recorder: ready\n", b"OLED_FRAME 128 64 " + frame.hex().encode() + b"\n")
        self.assertEqual(read_frame(device), frame)
        self.assertEqual(device.commands, [b"d"])

    def test_menu_state_query(self):
        device = FakeDevice(b"OLED_STATE 6 0 1 0 0\n")
        self.assertEqual(menu_state(device), (6, 0, 1, 0, 0))
        self.assertEqual(device.commands, [b"p"])

    def test_menu_state_retries_a_lost_reply(self):
        device = FakeDevice(b"", b"OLED_STATE 6 0 1 0 0\n")
        with patch("scripts.test_device.time.monotonic", side_effect=[0, 1, 6, 7, 8]):
            self.assertEqual(menu_state(device), (6, 0, 1, 0, 0))
        self.assertEqual(device.commands, [b"p", b"p"])

    def test_wifi_profile_is_open_and_has_correct_ssid(self):
        profile = ET.fromstring(wifi_profile("RadioRecorder"))
        namespace = {"w": "http://www.microsoft.com/networking/WLAN/profile/v1"}
        self.assertEqual(profile.findtext("w:SSIDConfig/w:SSID/w:name", namespaces=namespace), "RadioRecorder")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:authentication", namespaces=namespace), "open")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:encryption", namespaces=namespace), "none")


if __name__ == "__main__":
    unittest.main()