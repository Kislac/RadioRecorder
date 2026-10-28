import sys
import unittest
from contextlib import redirect_stdout
from itertools import count
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from tempfile import TemporaryDirectory
from unittest.mock import patch
import xml.etree.ElementTree as ET

from scripts.oled_viewer import FRAME_BYTES, KEY_COMMANDS, body_pixels, find_device, parse_frame, pixel, read_frame
from scripts.test_device import delete_test_recording, menu_state, valid_signal, validate_display_frames
from scripts.test_support import TestRun, firmware_at_least
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
        self.assertIn('device.write(b"w")', watchdog_test_source)
        self.assertIn("task watchdog got triggered", watchdog_test_source)
        self.assertIn('line == "Watchdog: ready (10 s)"', watchdog_test_source)

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
        self.assertIn("Kijelző felébresztése", page_source)
        self.assertIn("/api/display/wake", page_source)
        self.assertIn("oled.drawXBM(0, 0, 128, 64, kStartupImageBits)", display_source)
        self.assertIn("playbackPlayCue(PlaybackCue::startup)", (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn("playbackPlayCue(PlaybackCue::recordingStarted)", recorder_source)
        self.assertIn("playbackPlayCue(PlaybackCue::recordingStopped)", recorder_source)
        self.assertIn('static_assert(sizeof(kStartupImageBits) == 1024', startup_image)
        self.assertIn("sinf(phase)", playback_source)
        self.assertIn("if (speakerReady && xTaskCreatePinnedToCore", playback_source)
        self.assertIn("cuePlaying.compare_exchange_strong", playback_source)
        self.assertIn("cuePlaying.load() ? PlaybackState::playing", playback_source)
        self.assertIn('playbackPlayCue(PlaybackCue::startup)', (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn('String(__DATE__) + " " + String(__TIME__)', app_source)
        self.assertIn('Serial.printf("Build time: %s\\n"', (root / "src" / "main.cpp").read_text(encoding="utf-8"))
        self.assertIn('appFirmwareBuildDateTime()', display_source)
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
        self.assertIn('"Wi-Fi", "Beallitasok"', display_source)
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
        filename = "REC00042.wav"
        device = FakeDevice(b"Delete test WAV: send exact RECxxxxx.wav filename\n",
                            b"Test WAV deleted: REC00042.wav\n")
        self.assertEqual(delete_test_recording(device, filename), "removed " + filename)
        self.assertEqual(device.commands, [b"x", filename.encode() + b"\n"])

    def test_test_recording_delete_rejects_unexpected_path(self):
        device = FakeDevice()
        with self.assertRaises(ValueError):
            delete_test_recording(device, "../REC00042.wav")
        self.assertEqual(device.commands, [])

    def test_test_run_summary_is_saved_to_log(self):
        with TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            run = TestRun("offline test", log_dir=directory)
            self.assertTrue(run.case("frame check", lambda: "valid")[0])
            self.assertTrue(run.case("serial connection", lambda: SimpleNamespace(port="COM3"))[0])
            run.skip("recording", "opt-in")
            self.assertTrue(run.summary())
            log = run.log_path.read_text(encoding="utf-8")
        self.assertIn("PASS: frame check - valid", log)
        self.assertIn("PASS: serial connection - COM3", log)
        self.assertIn("SUMMARY: 2 passed, 0 failed, 1 skipped", log)

    def test_test_run_summary_marks_failures(self):
        with TemporaryDirectory() as directory, redirect_stdout(StringIO()):
            run = TestRun("offline test", log_dir=directory)
            self.assertFalse(run.case("broken check", lambda: 1 / 0)[0])
            self.assertFalse(run.summary())
            log = run.log_path.read_text(encoding="utf-8")
        self.assertIn("FAIL: broken check", log)
        self.assertIn("SUMMARY: 0 passed, 1 failed, 0 skipped", log)

    def test_signal_sample_must_be_in_adc_range(self):
        self.assertEqual(valid_signal(["GPIO1 RMS: 12 | Csucs: 30 | DC-kozep: 2010 (ADC)"]), (12, 30, 2010))
        for line in ("GPIO1 RMS: 31 | Csucs: 30 | DC-kozep: 2010 (ADC)",
                     "GPIO1 RMS: 12 | Csucs: 30 | DC-kozep: 5000 (ADC)",
                     "ADC meresi hiba (3)"):
            with self.subTest(line=line), self.assertRaises(AssertionError):
                valid_signal([line])

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
        self.assertEqual(commands["<Return>"], "k")
        self.assertEqual(commands["<space>"], "b")
        self.assertEqual(commands["<Escape>"], "b")

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

    def test_wifi_profile_is_open_and_has_correct_ssid(self):
        profile = ET.fromstring(wifi_profile("RadioRecorder"))
        namespace = {"w": "http://www.microsoft.com/networking/WLAN/profile/v1"}
        self.assertEqual(profile.findtext("w:SSIDConfig/w:SSID/w:name", namespaces=namespace), "RadioRecorder")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:authentication", namespaces=namespace), "open")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:encryption", namespaces=namespace), "none")


if __name__ == "__main__":
    unittest.main()