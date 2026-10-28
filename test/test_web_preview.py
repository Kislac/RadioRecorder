import http.client
import json
from pathlib import Path
import runpy
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.parse import urlencode

from scripts.preview_web import PreviewDevice, PreviewServer
from scripts.web_assets import AssetReferences, PAGES, WEB_ROOT, generate_header, render_page


class WebAssetTests(unittest.TestCase):
    def test_microphone_firmware_capture_contract(self):
        root = WEB_ROOT.parent
        recorder = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        self.assertIn("I2S_NUM_0", recorder)
        self.assertIn("GPIO_NUM_6", recorder)
        self.assertIn("GPIO_NUM_7", recorder)
        self.assertIn("GPIO_NUM_8", recorder)
        self.assertIn("I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO", recorder)
        self.assertIn("gpio_set_pull_mode(GPIO_NUM_8, GPIO_PULLDOWN_ONLY)", recorder)
        self.assertIn("i2s_channel_read(micHandle", recorder)
        self.assertIn("put16(pcmBuffer + pcmLength, static_cast<uint16_t>(pcm))", recorder)
        self.assertIn("memcpy(&sample, dmaBuffer + offset, sizeof(sample))", recorder)
        self.assertNotIn("energy[1] > energy[0]", recorder)
        self.assertIn("addMicSignalSample(window, pcm)", recorder)
        self.assertIn("if (enabled && micSignalEnabled.load()) return;", recorder)
        self.assertIn("writeWavHeader(file, dataBytes)", recorder)
        self.assertIn("liveAudioClient() < 0", control)
        self.assertIn("appStartMicRecording()", display)
        self.assertIn('server.on("/api/mic/start", HTTP_POST', server)
        self.assertIn('server.on("/api/mic/signal", HTTP_GET', server)

    def test_ptt_firmware_has_bounded_idle_first_control_path(self):
        root = WEB_ROOT.parent
        relay = (root / "src" / "ptt_test.cpp").read_text(encoding="utf-8")
        header = (root / "include" / "ptt_test.h").read_text(encoding="utf-8")
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        self.assertIn("kPttTestPulseMs = 1000", header)
        self.assertIn("kRelayGpio = 10", relay)
        self.assertIn("kRelayActiveLevel = HIGH", relay)
        self.assertLess(relay.index("digitalWrite(kRelayGpio, kRelayIdleLevel);"),
                        relay.index("pinMode(kRelayGpio, OUTPUT);"))
        self.assertIn("if (active && static_cast<uint32_t>(millis() - startedAt) >= kPttTestPulseMs) stopPttTest();", relay)
        self.assertIn("esp_timer_create(&timerArgs, &pulseTimer) == ESP_OK", relay)
        self.assertIn("esp_timer_start_once(pulseTimer, static_cast<uint64_t>(kPttTestPulseMs) * 1000)", relay)
        self.assertIn("timerArgs.dispatch_method = ESP_TIMER_TASK", relay)
        self.assertIn("digitalWrite(kRelayGpio, kRelayIdleLevel);\n    active = false;", relay)
        self.assertIn("if (!initialized || active) return false;", relay)
        self.assertIn("initPttTest();", main)
        self.assertIn("pollPttTest();", main)
        self.assertIn("if (!startPttTest())", display)
        self.assertIn('server.on("/api/ptt-test/on", HTTP_POST', server)
        self.assertIn('server.on("/api/ptt-test/off", HTTP_POST', server)

    def test_platformio_hook_generates_header_without_building_firmware(self):
        class FakeEnvironment:
            def __init__(self, project):
                self.project = project
                self.include_paths = []

            def subst(self, value):
                self.assert_variable = value
                return str(self.project)

            def Append(self, CPPPATH):
                self.include_paths.extend(CPPPATH)

        with tempfile.TemporaryDirectory() as directory:
            environment = FakeEnvironment(Path(directory))
            hook = WEB_ROOT.parent / "scripts" / "embed_web.py"
            runpy.run_path(str(hook), init_globals={"Import": lambda name: None, "env": environment})
            header = Path(directory) / ".pio/generated/web_pages.generated.h"
            self.assertTrue(header.is_file())
            self.assertEqual(environment.include_paths, [str(header.parent)])
            self.assertIn("kSettingsPage", header.read_text(encoding="utf-8"))

    def test_all_pages_bundle_their_actual_assets(self):
        for name in PAGES:
            with self.subTest(page=name):
                document = render_page(name)
                parser = AssetReferences()
                parser.feed((WEB_ROOT / (name + ".html")).read_text(encoding="utf-8"))
                for _, _, reference in parser.references:
                    self.assertIn((WEB_ROOT / reference.lstrip("/")).read_text(encoding="utf-8"), document)
                self.assertNotIn('<script src=', document)
                self.assertNotIn('rel="stylesheet"', document)
                self.assertNotIn("/preview/live", document)

    def test_header_preserves_public_symbols_and_skips_identical_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = generate_header(Path(directory) / "web_pages.generated.h")
            initial_time = path.stat().st_mtime_ns
            header = path.read_text(encoding="utf-8")
            for name, symbol in PAGES.items():
                self.assertIn('const char ' + symbol + '[] PROGMEM', header)
                self.assertIn(render_page(name), header)
            generate_header(path)
            self.assertEqual(path.stat().st_mtime_ns, initial_time)

    def test_asset_path_cannot_escape_source_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "index.html").write_text('<link rel="stylesheet" href="../secret.css">', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "escapes"):
                render_page("index", root)


class PreviewHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = PreviewServer(("127.0.0.1", 0))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.server.device = PreviewDevice()

    def request(self, path, method="GET", params=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        body = urlencode(params) if params is not None else None
        connection.request(method, path, body=body,
                           headers=headers or ({"Content-Type": "application/x-www-form-urlencoded"} if body else {}))
        response = connection.getresponse()
        data = response.read()
        status, response_headers = response.status, dict(response.getheaders())
        connection.close()
        return status, data, response_headers

    def test_pages_navigation_and_assets_are_available(self):
        for path, name in (("/", "index"), ("/files", "files"), ("/settings", "settings"), ("/test", "test")):
            with self.subTest(path=path):
                status, page, _ = self.request(path)
                self.assertEqual(status, 200)
                self.assertIn(b"[Offline]", page)
                self.assertIn(b"/preview/live", page)
                for asset_name in (("settings.css", "test.js") if name == "test" else (name + ".css", name + ".js")):
                    status, asset, _ = self.request('/' + asset_name)
                    self.assertEqual(status, 200)
                    self.assertGreater(len(asset), 100)
        self.assertEqual(self.request('/../platformio.ini')[0], 404)
        self.assertEqual(self.request('/test.css')[0], 404)

    def test_record_stop_and_audio_range(self):
        status, name, _ = self.request('/api/start', 'POST')
        self.assertEqual(status, 200)
        self.assertEqual(self.request('/api/files')[0], 409)
        self.assertEqual(self.request('/api/start', 'POST')[0], 409)
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        files = json.loads(self.request('/api/recordings')[1])["files"]
        self.assertEqual(files[0]["name"], name.decode())
        status, audio, headers = self.request('/file?name=' + name.decode(), headers={"Range": "bytes=0-43"})
        self.assertEqual(status, 206)
        self.assertEqual(len(audio), 44)
        self.assertEqual(audio[:4], b'RIFF')
        self.assertIn('Content-Range', headers)
        self.assertEqual(self.request('/file?name=' + name.decode(), headers={"Range": "bytes=999999-"})[0], 416)

    def test_microphone_recording_uses_same_wav_and_excludes_adc(self):
        self.assertTrue(json.loads(self.request('/api/status')[1])["micReady"])
        status, name, _ = self.request('/api/mic/start', 'POST')
        self.assertEqual(status, 200)
        state = json.loads(self.request('/api/status')[1])
        self.assertEqual((state["state"], state["recordSource"]), ("recording", "microphone"))
        self.assertEqual(self.request('/api/start', 'POST')[0], 409)
        self.assertEqual(self.request('/api/mic/start', 'POST')[0], 409)
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        self.assertEqual(json.loads(self.request('/api/recordings')[1])["files"][0]["name"], name.decode())
        self.assertEqual(self.request('/file?name=' + name.decode())[1][:4], b'RIFF')
        self.server.device = PreviewDevice("missing-sd")
        self.assertFalse(json.loads(self.request('/api/status')[1])["micReady"])
        self.assertEqual(self.request('/api/mic/start', 'POST')[0], 409)

    def test_microphone_meter_idle_recording_and_shutdown(self):
        files_before = dict(self.server.device.files)
        self.assertEqual(self.request('/api/mic/signal/on', 'POST')[0], 200)
        meter = json.loads(self.request('/api/mic/signal')[1])
        self.assertTrue(meter['enabled'] and meter['active'])
        self.assertGreater(meter['peak'], meter['rms'])
        self.assertEqual(self.server.device.files, files_before)
        self.assertEqual(self.request('/api/signal/on', 'POST')[0], 409)
        self.assertEqual(self.request('/api/mic/start', 'POST')[0], 200)
        self.assertTrue(json.loads(self.request('/api/mic/signal')[1])['active'])
        self.assertEqual(self.request('/api/mic/signal/off', 'POST')[0], 200)
        self.assertFalse(json.loads(self.request('/api/mic/signal')[1])['enabled'])
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        self.assertEqual(self.request('/api/mic/signal/on', 'POST')[0], 200)
        self.server.device.mic_signal_polled_at -= 5
        self.assertFalse(json.loads(self.request('/api/mic/signal')[1])['enabled'])
        self.assertEqual(self.request('/api/signal/on', 'POST')[0], 200)
        self.assertEqual(self.request('/api/mic/signal/on', 'POST')[0], 409)
        self.server.device = PreviewDevice('missing-sd')
        self.assertEqual(self.request('/api/mic/signal/on', 'POST')[0], 503)

    def test_settings_validate_before_mutation(self):
        self.assertEqual(self.request('/api/settings', 'POST', {"screenOffMinutes": "0", "wifiAutoOffMinutes": "999"})[0], 200)
        status, _, _ = self.request('/api/settings', 'POST', {"screenOffMinutes": "5", "wifiAutoOffMinutes": "1000"})
        self.assertEqual(status, 400)
        settings = json.loads(self.request('/api/settings')[1])
        self.assertEqual(settings["screenOffMinutes"], 0)
        self.assertEqual(settings["wifiAutoOffMinutes"], 999)

    def test_ptt_test_requires_explicit_pulse_and_expires_without_extension(self):
        status = lambda: json.loads(self.request('/api/ptt-test')[1])
        self.assertEqual(status(), {"active": False, "pulseMs": 1000})
        self.assertEqual(self.request('/api/ptt-test/on')[0], 404)
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 200)
        deadline = self.server.device.ptt_until
        self.assertTrue(status()["active"])
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 409)
        self.assertEqual(self.server.device.ptt_until, deadline)
        self.server.device.ptt_until = time.monotonic() - 0.01
        self.assertFalse(status()["active"])
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 200)
        self.assertEqual(self.request('/api/ptt-test/off', 'POST')[0], 200)
        self.assertFalse(status()["active"])

    def test_signal_and_network_operations(self):
        self.assertEqual(self.request('/api/signal/on', 'POST')[0], 200)
        signal = json.loads(self.request('/api/signal')[1])
        self.assertTrue(signal["active"])
        self.assertGreater(signal["peak"], signal["rms"])
        self.assertEqual(self.request('/api/wifi/scan', 'POST')[0], 202)
        self.server.device.scan_started -= 1
        self.assertEqual(len(json.loads(self.request('/api/wifi/scan')[1])["networks"]), 2)
        self.assertEqual(self.request('/api/wifi/connect', 'POST', {"ssid": "Workshop WiFi", "password": "dummy", "keepApHost": "1"})[0], 202)
        deadline, ssid, keep = self.server.device.pending_network
        self.server.device.pending_network = (deadline - 2, ssid, keep)
        wifi = json.loads(self.request('/api/wifi/status')[1])
        self.assertEqual(wifi["mode"], "AP+STA")
        self.assertNotIn("password", wifi)
        self.assertEqual(self.request('/api/wifi/forget', 'POST')[0], 200)
        self.assertFalse(json.loads(self.request('/api/wifi/status')[1])["hasSavedNetwork"])

    def test_missing_sd_scenario(self):
        self.server.device = PreviewDevice("missing-sd")
        self.assertFalse(json.loads(self.request('/api/status')[1])["ready"])
        self.assertEqual(self.request('/api/start', 'POST')[0], 409)

    def test_failed_connection_scenario_does_not_save_network(self):
        device = PreviewDevice("wifi-failure")
        self.server.device = device
        self.request('/api/wifi/connect', 'POST', {"ssid": "Unavailable network"})
        deadline, ssid, keep = device.pending_network
        device.pending_network = (deadline - 2, ssid, keep)
        wifi = json.loads(self.request('/api/wifi/status')[1])
        self.assertEqual(wifi["mode"], "AP")
        self.assertFalse(wifi["hasSavedNetwork"])


if __name__ == "__main__":
    unittest.main()