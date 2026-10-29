from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from scripts.radio_programmer import RadioClient, load_patch, main, save_snapshot


class SerialDevice:
    def __init__(self, handler):
        self.handler = handler
        self.pending = b""
        self.commands = []

    def write(self, data):
        request = json.loads(data.strip().removeprefix(b"@RR "))
        self.commands.append(request)
        result = self.handler(request)
        self.pending += b"Recorder: background log\nRR_REPLY 999999 {}\n"
        self.pending += b"RR_REPLY " + str(request["id"]).encode() + b" " + json.dumps(result).encode() + b"\n"
        return len(data)

    def read(self, count):
        output, self.pending = self.pending[:min(count, 7)], self.pending[min(count, 7):]
        return output


class RadioClientTests(unittest.TestCase):
    def test_read_task_prints_pretty_json_and_saves_snapshot(self):
        state = {"state": "done", "revision": "12345678", "settings": {"squelch": 5, "enabled": True},
                 "channels": [{"number": 0, "name": "Station"}]}
        device = Mock(port="COM3")
        client = Mock()
        client.request.return_value = {"protocol": 1}
        client.read_radio.return_value = state
        client.image.return_value = bytes(6472)
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "backup"
            with patch("scripts.radio_programmer.find_device", return_value=device), \
                    patch("scripts.radio_programmer.RadioClient", return_value=client), redirect_stdout(output):
                result = main(["--log-dir", temporary, "--port", "COM3", "read", "--confirm-read", "READ UV5R",
                               "--output", str(directory)])
            self.assertEqual(result, 0)
            self.assertIn("PASS: read radio - " + json.dumps(state, ensure_ascii=False, indent=2), output.getvalue())
            self.assertNotIn("'settings':", output.getvalue())
            self.assertEqual(json.loads((directory / "radio.json").read_text(encoding="utf-8")), state)
            self.assertEqual((directory / "radio.img").stat().st_size, 6472)
            self.assertIn('"enabled": true', output.getvalue())
            log = next(Path(temporary).glob("*.log")).read_text(encoding="utf-8")
            self.assertIn('"settings": {\n    "squelch": 5,', log)
        device.close.assert_called_once()

    def test_fragmented_replies_ignore_background_and_stale_ids(self):
        device = SerialDevice(lambda request: {"protocol": 1})
        self.assertEqual(RadioClient(device).request("hello"), {"protocol": 1})
        self.assertEqual(device.commands[0]["command"], "hello")

    def test_response_errors_and_frame_limits(self):
        client = RadioClient(SerialDevice(lambda request: {"error": "Stale revision"}))
        with self.assertRaisesRegex(RuntimeError, "Stale revision"):
            client.request("write")
        with self.assertRaises(ValueError):
            client.request("write", payload="x" * 5000)

    def test_image_download_checks_revision_and_preserves_binary(self):
        image = bytes(range(256)) * 25 + bytes(range(72))
        def handler(request):
            offset, count = request["offset"], request["count"]
            return {"offset": offset, "total": len(image), "revision": "12345678",
                    "data": image[offset:offset + count].hex()}
        self.assertEqual(RadioClient(SerialDevice(handler)).image("12345678"), image)
        with self.assertRaisesRegex(ValueError, "snapshot changed"):
            RadioClient(SerialDevice(handler)).image("deadbeef")

    def test_snapshot_is_saved_without_overwriting(self):
        device = SerialDevice(lambda request: {"offset": request["offset"], "total": 6472,
                                               "revision": "12345678", "data": "00" * request["count"]})
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "backup"
            save_snapshot(RadioClient(device), {"revision": "12345678"}, directory)
            self.assertEqual((directory / "radio.img").stat().st_size, 6472)
            self.assertEqual(json.loads((directory / "patch.json").read_text())["changes"], [])
            with self.assertRaises(FileExistsError):
                save_snapshot(RadioClient(device), {"revision": "12345678"}, directory)

    def test_write_and_read_require_opt_in_before_opening_port(self):
        with tempfile.TemporaryDirectory() as temporary, patch("scripts.radio_programmer.find_device") as finder:
            self.assertEqual(main(["--log-dir", temporary, "read"]), 2)
            self.assertEqual(main(["--log-dir", temporary, "write", "--patch", "missing.json"]), 2)
            finder.assert_not_called()
            self.assertEqual(len(list(Path(temporary).glob("*.log"))), 2)

    def test_patch_validation_rejects_bad_roots_and_channel_indices(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "patch.json"
            for content in ({"revision": "12345678", "changes": []},
                            {"revision": "wrong", "changes": [{}]},
                            {"revision": "12345678", "changes": [{"group": "channel", "field": "powerRaw", "value": 0, "index": True}]}):
                path.write_text(json.dumps(content))
                with self.assertRaises(ValueError):
                    load_patch(path)
            content = {"revision": "12345678", "changes": [{"group": "settings", "field": "squelch", "value": 4}]}
            path.write_text(json.dumps(content))
            self.assertEqual(load_patch(path), content)