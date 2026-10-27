import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from oled_viewer import FRAME_BYTES, body_pixels, parse_frame, pixel, read_frame
from test_device import menu_state
from test_wifi import wifi_profile
import xml.etree.ElementTree as ET


class FakeDevice:
    def __init__(self, *lines):
        self.lines = iter(lines)
        self.commands = []

    def write(self, command):
        self.commands.append(command)

    def readline(self):
        return next(self.lines)


class FrameTests(unittest.TestCase):
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