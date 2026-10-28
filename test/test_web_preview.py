import http.client
import io
import json
from pathlib import Path
import runpy
import tempfile
import threading
import time
import unittest
import zipfile
from unittest.mock import patch
from urllib.parse import urlencode

from scripts.preview_web import PreviewDevice, PreviewServer
from scripts.web_assets import AssetReferences, PAGES, WEB_ROOT, generate_header, render_page


class WebAssetTests(unittest.TestCase):
    def test_file_manager_pages_folders_before_files_and_loads_on_scroll(self):
        server = (WEB_ROOT.parent / "src" / "web_server.cpp").read_text(encoding="utf-8")
        script = (WEB_ROOT / "files.js").read_text(encoding="utf-8")
        self.assertIn('folder != (pass == 0)', server)
        self.assertIn('if (index++ < offset)', server)
        self.assertIn('new IntersectionObserver(', script)
        self.assertIn('entries[0].isIntersecting', script)
        self.assertIn('if (!loading && !loadFailed && more.getBoundingClientRect()', script)

    def test_dated_recording_and_file_api_firmware_contract(self):
        root = WEB_ROOT.parent
        recorder = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        files_script = (WEB_ROOT / "files.js").read_text(encoding="utf-8")
        self.assertIn('static constexpr time_t kMinEpoch = 946684800;', control)
        self.assertIn('clockDefault ? "default" : "saved"', control)
        self.assertIn('SD_MMC.mkdir(folder)', recorder)
        self.assertIn('"%Y-%m-%d"', recorder)
        self.assertIn('"MicMan" : "RadioMan"', recorder)
        self.assertIn('"%y%m%d_%H%M%S"', recorder)
        self.assertIn('server.on("/api/files/delete", HTTP_POST, deleteSdEntry)', server)
        self.assertIn('server.on("/api/files/download", HTTP_GET, downloadSdFolder)', server)
        self.assertIn('server.arg("confirm") != "1"', server)
        self.assertIn('confirm(`Biztosan törlöd', files_script)
        self.assertIn('radio.disabled = true', files_script)

    def test_clock_firmware_contract(self):
        root = WEB_ROOT.parent
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        wifi = (root / "src" / "wifi_manager.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        self.assertIn("CET-1CEST,M3.5.0/2,M10.5.0/3", control)
        self.assertIn('preferences.putULong64(kEpochKey', control)
        self.assertIn('preferences.getULong64(kEpochKey', control)
        self.assertIn('sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED', control)
        self.assertIn('if (clockDay(now) != savedDay) saveClock(now)', control)
        self.assertIn('appClockStartSync();', wifi)
        self.assertIn('appClockStopSync();', wifi)
        self.assertIn('case 12: oled.drawStr(9, y, "Idozona")', display)
        self.assertIn('if (!appClockSetLocal(value))', display)
        self.assertIn('server.on("/api/time", HTTP_POST, setClock)', server)
        self.assertIn('server.on("/api/time/zone", HTTP_POST, setClockZone)', server)

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
        self.assertIn("kRelayActiveLevel = LOW", relay)
        self.assertIn("kRelayIdleLevel = HIGH", relay)
        self.assertLess(relay.index("gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);"),
                relay.index("pinMode(kRelayGpio, OUTPUT);"))
        self.assertLess(relay.index("pinMode(kRelayGpio, OUTPUT);"),
                relay.index("digitalWrite(kRelayGpio, kRelayIdleLevel);"))
        self.assertIn("if (active && !latched && static_cast<uint32_t>(millis() - startedAt) >= kPttTestPulseMs) stopPttTest();", relay)
        self.assertIn("esp_timer_create(&timerArgs, &pulseTimer) == ESP_OK", relay)
        self.assertIn("esp_timer_start_once(pulseTimer, static_cast<uint64_t>(kPttTestPulseMs) * 1000)", relay)
        self.assertIn("timerArgs.dispatch_method = ESP_TIMER_TASK", relay)
        self.assertIn("digitalWrite(kRelayGpio, kRelayIdleLevel);\n    active = false;", relay)
        self.assertIn("if (!initialized || active) return false;", relay)
        self.assertIn("initPttTest();", main)
        self.assertLess(main.index("initPttTest();", main.index("void setup()")),
                main.index("Serial.begin(115200);", main.index("void setup()")))
        self.assertIn("if (!initialized || active) return false;\n  latched = true;", relay)
        self.assertIn("pollPttTest();", main)
        self.assertIn("if (!startPttTest())", display)
        self.assertIn('server.on("/api/ptt-test/on", HTTP_POST', server)
        self.assertIn('server.on("/api/ptt-test/latch", HTTP_POST', server)
        self.assertIn('server.on("/api/ptt-test/off", HTTP_POST', server)
        test_page = (WEB_ROOT / "test.html").read_text(encoding="utf-8")
        test_script = (WEB_ROOT / "test.js").read_text(encoding="utf-8")
        self.assertIn('id="pttToggle" type="button" aria-pressed="false"', test_page)
        self.assertIn('pttActive ? "/api/ptt-test/off" : "/api/ptt-test/latch"', test_script)
        self.assertIn('pttToggle.setAttribute("aria-pressed", String(state.active))', test_script)

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
        self.assertEqual(files[0]["path"], name.decode())
        self.assertRegex(name.decode(), r'^/2000-01-01/RadioMan000101_\d{6}\.wav$')
        status, audio, headers = self.request('/file?path=' + name.decode(), headers={"Range": "bytes=0-43"})
        self.assertEqual(status, 206)
        self.assertEqual(len(audio), 44)
        self.assertEqual(audio[:4], b'RIFF')
        self.assertIn('Content-Range', headers)
        self.assertEqual(self.request('/file?path=' + name.decode(), headers={"Range": "bytes=999999-"})[0], 416)

    def test_microphone_recording_uses_same_wav_and_excludes_adc(self):
        self.assertTrue(json.loads(self.request('/api/status')[1])["micReady"])
        status, name, _ = self.request('/api/mic/start', 'POST')
        self.assertEqual(status, 200)
        state = json.loads(self.request('/api/status')[1])
        self.assertEqual((state["state"], state["recordSource"]), ("recording", "microphone"))
        self.assertEqual(self.request('/api/start', 'POST')[0], 409)
        self.assertEqual(self.request('/api/mic/start', 'POST')[0], 409)
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        self.assertRegex(name.decode(), r'^/2000-01-01/MicMan000101_\d{6}\.wav$')
        self.assertEqual(json.loads(self.request('/api/recordings')[1])["files"][0]["path"], name.decode())
        self.assertEqual(self.request('/file?path=' + name.decode())[1][:4], b'RIFF')
        self.server.device = PreviewDevice("missing-sd")
        self.assertFalse(json.loads(self.request('/api/status')[1])["micReady"])
        self.assertEqual(self.request('/api/mic/start', 'POST')[0], 409)

    def test_dated_folder_zip_and_confirmed_delete(self):
        self.assertEqual(self.request('/api/time')[0], 200)
        for endpoint in ('/api/start', '/api/mic/start'):
            name = self.request(endpoint, 'POST')[1].decode()
            self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
            self.assertIn(name, self.server.device.files)
        items = json.loads(self.request('/api/files')[1])['items']
        self.assertIn('/2000-01-01', [item['path'] for item in items if item['directory']])
        folder = json.loads(self.request('/api/files?dir=/2000-01-01')[1])['items']
        self.assertEqual({item['path'] for item in folder}, {item for item in self.server.device.files
                                                      if item.startswith('/2000-01-01/')})
        status, archive, headers = self.request('/api/files/download?path=/2000-01-01')
        self.assertEqual((status, headers['Content-Type']), (200, 'application/zip'))
        with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
            self.assertEqual(set(zipped.namelist()), {'2000-01-01/' + item['name'] for item in folder})
            self.assertTrue(all(zipped.read(item).startswith(b'RIFF') for item in zipped.namelist()))
        self.assertEqual(self.request('/api/files/delete', 'POST', {'path': '/2000-01-01'})[0], 400)
        self.assertEqual(self.request('/api/files/delete', 'POST', {'path': '/', 'confirm': '1'})[0], 400)
        self.assertEqual(self.request('/api/files/delete', 'POST', {'path': '/../Archive', 'confirm': '1'})[0], 400)
        self.assertEqual(self.request('/api/files/delete', 'POST', {'path': '/2000-01-01', 'confirm': '1'})[0], 200)
        self.assertEqual(self.request('/api/files?dir=/2000-01-01')[0], 404)
        self.assertEqual(self.request('/api/files/download?path=/2000-01-01')[0], 400)
        self.assertEqual(self.request('/api/recordings')[0], 200)

    def test_folders_precede_files_across_page_boundaries(self):
        device = self.server.device
        for number in range(22):
            device.directories.add(f'/Folder{number:02d}')
        for number in range(25):
            device.files[f'/Sample{number:02d}.wav'] = device.wav
        pages = [json.loads(self.request(f'/api/files?offset={offset}')[1])
                 for offset in (0, 20, 40)]
        self.assertEqual([page['next'] for page in pages], [20, 40, None])
        self.assertEqual([len(page['items']) for page in pages], [20, 20, 10])
        items = [item for page in pages for item in page['items']]
        self.assertTrue(all(item['directory'] for item in items[:23]))
        self.assertTrue(all(not item['directory'] for item in items[23:]))
        self.assertEqual(len({item['path'] for item in items}), 50)

    def test_same_second_names_do_not_overwrite_and_busy_delete_is_rejected(self):
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-10-28T23:25'})[0], 200)
        first = self.request('/api/start', 'POST')[1].decode()
        self.assertEqual(self.request('/api/files/delete', 'POST',
                                      {'path': '/2026-10-28', 'confirm': '1'})[0], 409)
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        second = self.request('/api/start', 'POST')[1].decode()
        self.assertEqual(self.request('/api/stop', 'POST')[0], 200)
        self.assertEqual(second, first[:-4] + '_01.wav')
        self.assertEqual(self.request('/api/files/delete', 'POST', {'path': first, 'confirm': '1'})[0], 200)
        self.assertNotIn(first, self.server.device.files)
        self.assertIn(second, self.server.device.files)

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

    def test_clock_manual_timezone_dst_and_restored_date(self):
        clock = lambda: json.loads(self.request('/api/time')[1])
        self.assertEqual((clock()['source'], clock()['local'][:10]), ('default', '2000-01-01'))
        self.assertEqual(clock()['zone'], 0)
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-02-29T12:00'})[0], 400)
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-03-29T02:30'})[0], 400)
        self.assertEqual(self.request('/api/time/zone', 'POST', {'zone': '29'})[0], 400)
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-07-01T12:34'})[0], 200)
        self.assertEqual((clock()['local'][:16], clock()['source']), ('2026-07-01T12:34', 'manual'))
        self.assertEqual(self.request('/api/time/zone', 'POST', {'zone': '1'})[0], 200)
        self.assertEqual(clock()['local'][:16], '2026-07-01T10:34')
        self.assertEqual(self.request('/api/time/zone', 'POST', {'zone': '17'})[0], 200)
        self.assertEqual(clock()['local'][:16], '2026-07-01T13:34')
        saved = self.server.device.saved_epoch, self.server.device.clock_zone
        self.server.device = PreviewDevice(saved_clock=saved)
        self.assertEqual((clock()['source'], clock()['zone'], clock()['local'][:16]),
                         ('saved', 17, '2026-07-01T13:34'))
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-07-02T23:59'})[0], 200)
        self.server.device.clock_started_at -= 120
        self.assertEqual(clock()['local'][:10], '2026-07-03')
        self.assertEqual(self.server.device.saved_day.isoformat(), '2026-07-03')

    def test_clock_reports_sync_only_after_wifi_success(self):
        clock = lambda: json.loads(self.request('/api/time')[1])
        self.assertEqual(self.request('/api/wifi/connect', 'POST',
                                      {'ssid': 'Workshop WiFi', 'password': 'dummy', 'keepApHost': '1'})[0], 202)
        self.assertEqual(clock()['source'], 'default')
        deadline, ssid, keep = self.server.device.pending_network
        self.server.device.pending_network = (deadline - 2, ssid, keep)
        self.assertEqual(clock()['source'], 'internet')
        self.assertEqual(self.request('/api/time', 'POST', {'local': '2026-01-02T12:00'})[0], 200)
        self.assertEqual(clock()['source'], 'manual')
        self.assertEqual(self.request('/api/wifi/forget', 'POST')[0], 200)
        self.assertEqual(clock()['source'], 'manual')

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

    def test_ptt_latch_stays_on_until_explicitly_switched_off(self):
        status = lambda: json.loads(self.request('/api/ptt-test')[1])
        self.assertEqual(self.request('/api/ptt-test/latch')[0], 404)
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 200)
        self.assertEqual(self.request('/api/ptt-test/latch', 'POST')[0], 409)
        self.assertEqual(self.request('/api/ptt-test/off', 'POST')[0], 200)
        self.assertEqual(self.request('/api/ptt-test/latch', 'POST')[0], 200)
        self.assertTrue(status()['active'])
        self.assertEqual(self.request('/api/ptt-test/latch', 'POST')[0], 409)
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 409)
        self.server.device.ptt_until = time.monotonic() - 2
        self.assertTrue(status()['active'])
        self.assertEqual(self.request('/api/ptt-test/off', 'POST')[0], 200)
        self.assertFalse(status()['active'])
        self.assertEqual(self.request('/api/ptt-test/off', 'POST')[0], 200)

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