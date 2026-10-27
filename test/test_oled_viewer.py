import sys
import unittest
from contextlib import redirect_stdout
from itertools import count
from io import StringIO
from types import SimpleNamespace
from tempfile import TemporaryDirectory
from unittest.mock import patch
import xml.etree.ElementTree as ET

from scripts.oled_viewer import FRAME_BYTES, KEY_COMMANDS, body_pixels, find_device, parse_frame, pixel, read_frame
from scripts.test_device import delete_test_recording, menu_state, valid_signal
from scripts.test_support import TestRun
from scripts.test_wifi import wifi_profile


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
        esp = FakeDevice(b"Firmware: v0.1.24\n", b"Recorder: ready\n", b"Wi-Fi AP: unavailable\n")
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
        device = FakeDevice(b"OLED_STATE 6 0 1 0\n")
        self.assertEqual(menu_state(device), (6, 0, 1, 0))
        self.assertEqual(device.commands, [b"p"])

    def test_wifi_profile_is_open_and_has_correct_ssid(self):
        profile = ET.fromstring(wifi_profile("RadioRecorder"))
        namespace = {"w": "http://www.microsoft.com/networking/WLAN/profile/v1"}
        self.assertEqual(profile.findtext("w:SSIDConfig/w:SSID/w:name", namespaces=namespace), "RadioRecorder")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:authentication", namespaces=namespace), "open")
        self.assertEqual(profile.findtext("w:MSM/w:security/w:authEncryption/w:encryption", namespaces=namespace), "none")


if __name__ == "__main__":
    unittest.main()