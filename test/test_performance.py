import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch
import json

from scripts.test_support import TestRun, validate_performance_snapshot
from scripts.preview_web import PreviewDevice
from scripts.test_device import performance_preflight, performance_postcheck
from scripts.test_wifi import main as wifi_main, recording_metadata_found, recording_metadata_on_sd, test_performance_switch


class PerformanceDiagnosticsTests(unittest.TestCase):
    def test_enabled_snapshot_requires_memory_tasks_nvs_and_audio_counters(self):
        snapshot = {key: 1 for key in (
            "uptimeMs", "loopMaxGapUs", "loopGapsOver100ms", "taskCount",
            "loopCurrentUs", "loopAverageUs", "loopPeakUs", "loopSamples",
            "displayCurrentUs", "displayAverageUs", "displayPeakUs", "displaySamples",
            "logBytes", "logSamples", "logSkippedBusy", "logBuildUs", "logWriteUs",
            "loopStackFreeMinimum", "recorderStackFreeMinimum", "speakerStackFreeMinimum",
            "lastFileWorkerStackFreeMinimum", "adcOverflows", "liveAudioDroppedFrames",
            "internalFree", "internalTotal", "internalMinimumFree", "internalLargestBlock",
            "psramFree", "psramTotal", "psramMinimumFree", "psramLargestBlock",
            "nvsTotalEntries", "nvsUsedEntries", "nvsFreeEntries")}
        snapshot.update(enabled=True, taskCount=5, nvsTotalEntries=10, nvsFreeEntries=4,
                logEnabled=False, logPath="", logError="")
        self.assertIn("4 NVS entries free", validate_performance_snapshot(snapshot))
        for field in ("nvsFreeEntries", "recorderStackFreeMinimum", "adcOverflows", "loopMaxGapUs"):
            with self.subTest(field=field):
                broken = snapshot.copy()
                del broken[field]
                with self.assertRaises(AssertionError):
                    validate_performance_snapshot(broken)

    def test_disabled_snapshot_has_no_metrics(self):
        self.assertEqual(validate_performance_snapshot({"enabled": False}), "disabled (persistent setting)")
        with self.assertRaises(AssertionError):
            validate_performance_snapshot({"enabled": False, "internalFree": 12})

    def test_settings_offer_explicit_performance_switch(self):
        root = Path(__file__).resolve().parents[1]
        page = (root / "web" / "settings.html").read_text(encoding="utf-8")
        script = (root / "web" / "settings.js").read_text(encoding="utf-8")
        self.assertIn('id="performanceEnabled" type="checkbox"', page)
        self.assertIn('id="performanceSnapshot"', page)
        self.assertIn('fetch("/api/performance?enabled="', script)
        self.assertIn('fetch("/api/performance",', script)
        self.assertIn('id="diagnostics"', page)
        self.assertIn('id="performanceTimings"', page)
        self.assertIn('id="performanceLogEnabled"', page)
        self.assertIn('fetch("/api/performance/log?enabled="', script)
        self.assertIn('id="performanceLogViewer"', page)
        self.assertIn('id="performanceLogFiles"', page)
        self.assertIn('id="performanceLive"', page)
        self.assertIn('fetch("/api/performance/logs"', script)
        self.assertIn('function usedPercent(free, total)', script)
        self.assertIn('help.title = note', script)
        self.assertIn('addMetric("Last SD log write"', script)

    def test_log_api_only_lists_safe_bounded_files(self):
        root = Path(__file__).resolve().parents[1]
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        logger = (root / "src" / "performance.cpp").read_text(encoding="utf-8")
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('server.on("/api/performance/logs", HTTP_GET', server)
        self.assertIn('performanceLogValidPath(path)', server)
        self.assertIn('file.size() > 256 * 1024', server)
        self.assertIn('constexpr uint16_t kLogMaxFiles = 128', logger)
        self.assertIn('SD_MMC.rename(oldPath, destination)', logger)
        self.assertIn('SD_MMC.exists(destination)', logger)
        self.assertIn('if (logDirectoryReady) return true;', logger)
        self.assertIn('if (!prepareLogDirectory()) { autoLogPending = false; return; }', logger)
        self.assertIn('SD_MMC.open(kPerformanceLogDirectory, FILE_READ)', server)
        self.assertIn('String(kPerformanceLogDirectory) + "/" + name.substring', server)
        self.assertIn('preferences.getBool("perfLogEnabled", true)', logger)
        self.assertIn('bootLogCountReady ? bootLogCount : logFileCount()', logger)
        self.assertLess(main.index('performancePrimeLogCount();'), main.index('void loop()'))

    def test_preview_on_off_and_invalid_value(self):
        device = PreviewDevice()
        code, snapshot = device.api("GET", "/api/performance", {})
        self.assertEqual(code, 200)
        self.assertTrue(snapshot["preview"])
        validate_performance_snapshot(snapshot)
        self.assertEqual(device.api("POST", "/api/performance", {"enabled": "2"})[0], 400)
        code, snapshot = device.api("POST", "/api/performance", {"enabled": "0"})
        self.assertEqual((code, snapshot), (200, {"enabled": False}))
        self.assertFalse(device.api("GET", "/api/settings", {})[1]["performanceEnabled"])
        self.assertTrue(device.api("POST", "/api/performance", {"enabled": "1"})[1]["enabled"])

    def test_preview_sd_logs_default_on_and_allow_safe_browsing(self):
        device = PreviewDevice()
        self.assertTrue(device.performance_log_enabled)
        code, listing = device.api("GET", "/api/performance/logs", {})
        self.assertEqual(code, 200)
        self.assertEqual(len(listing["files"]), 2)
        older = listing["files"][0]["path"]
        self.assertTrue(all(entry["path"].startswith("/performance-logs/perf-") for entry in listing["files"]))
        self.assertIn("/performance-logs", device.directories)
        self.assertIn(older, device.files)
        code, folder = device.api("GET", "/api/files", {"dir": "/performance-logs"})
        self.assertEqual(code, 200)
        self.assertIn(older, [entry["path"] for entry in folder["items"]])
        code, content = device.api("GET", "/api/performance/log", {"path": older})
        self.assertEqual(code, 200)
        self.assertGreater(json.loads(content)["metrics"]["loopSamples"], 0)
        self.assertEqual(device.api("GET", "/api/performance/log", {"path": "/other.txt"})[0], 400)
        self.assertEqual(device.api("GET", "/api/performance/log", {"path": "/perf-../secret.txt"})[0], 400)
        self.assertEqual(device.api("GET", "/api/performance/log", {"path": older.replace("/performance-logs", "")})[0], 400)
        self.assertEqual(device.api("POST", "/api/performance/log", {"enabled": "invalid"})[0], 400)
        self.assertFalse(device.api("POST", "/api/performance/log", {"enabled": "0"})[1]["logEnabled"])
        self.assertEqual(device.api("POST", "/api/performance", {"enabled": "0"})[1], {"enabled": False})
        code, historical = device.api("GET", "/api/performance/log", {"path": older})
        self.assertEqual(code, 200)
        self.assertTrue(json.loads(historical)["metrics"]["enabled"])
        self.assertEqual(device.api("POST", "/api/performance/log", {"enabled": "1"})[0], 409)
        self.assertTrue(PreviewDevice().performance_log_enabled)

    def test_serial_diagnostics_preflight_and_postcheck_require_real_samples(self):
        snapshot = PreviewDevice().api("GET", "/api/performance", {})[1]

        class Device:
            def __init__(self):
                self.commands = []

            def write(self, command):
                self.commands.append(command)

        device = Device()
        responses = [["PERFORMANCE_ENABLE enabled"], ["PERFORMANCE " + json.dumps(snapshot)],
                     ["PERFORMANCE " + json.dumps(snapshot)]]
        with patch("scripts.test_device.read_until", side_effect=responses):
            self.assertIn("loop", performance_preflight(device, None))
            self.assertIn("display", performance_postcheck(device, None))
        self.assertEqual(device.commands, [b"Y", b"V", b"V"])
        snapshot["loopSamples"] = 0
        with patch("scripts.test_device.read_until", return_value=["PERFORMANCE " + json.dumps(snapshot)]):
            with self.assertRaisesRegex(AssertionError, "No loop or display"):
                performance_postcheck(device, None)

    def test_firmware_exposes_serial_enable_without_sd_logging(self):
        source = (Path(__file__).resolve().parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("if (command == 'Y')", source)
        self.assertIn("performanceSetEnabled(true)", source)
        self.assertNotIn("performanceLogSetEnabled(true)", source)

    def test_wifi_suite_preserves_diagnostics_when_requested(self):
        with TemporaryDirectory() as folder, patch("scripts.test_wifi.TestRun", side_effect=lambda name: TestRun(name, folder)):
            with patch("scripts.test_wifi.test_api", return_value={"version": "0.1.92"}), \
                    patch("scripts.test_wifi.test_record_and_delete", return_value="test file deleted"), \
                    patch("scripts.test_wifi.test_performance_switch", return_value="restored") as toggle:
                with patch("sys.argv", ["test_wifi", "--record", "--preserve-performance"]):
                    self.assertEqual(wifi_main(), 0)
                    toggle.assert_not_called()
                with patch("sys.argv", ["test_wifi", "--record"]):
                    self.assertEqual(wifi_main(), 0)
                    toggle.assert_called_once()

    def test_write_test_restores_original_even_after_bad_response(self):
        calls = []

        def fake_request(_base_url, path, method="GET"):
            calls.append((path, method))
            if len(calls) == 1:
                return json.dumps({"enabled": True}).encode()
            if len(calls) == 2:
                return json.dumps({"enabled": True}).encode()
            return json.dumps({"enabled": True}).encode()

        class Report:
            def detail(self, _message):
                pass

        with patch("scripts.test_wifi.request", side_effect=fake_request):
            with self.assertRaises(AssertionError):
                test_performance_switch("http://device", Report())
        self.assertEqual(calls, [("/api/performance", "GET"),
                                 ("/api/performance?enabled=0", "POST"),
                                 ("/api/performance?enabled=1", "POST")])

    def test_recording_metadata_uses_full_sd_path(self):
        path = "/2026-11-03/RadioMan261103_183319.wav"
        files = [{"name": "RadioMan261103_183319.wav", "path": path, "bytes": 66092}]
        self.assertTrue(recording_metadata_found(files, path))
        self.assertFalse(recording_metadata_found(files, "/other/RadioMan261103_183319.wav"))
        self.assertFalse(recording_metadata_found([{**files[0], "bytes": 43}], path))

        with patch("scripts.test_wifi.request", return_value=json.dumps({"items": files}).encode()) as get:
            self.assertTrue(recording_metadata_on_sd("http://device", path))
        get.assert_called_once_with("http://device", "/api/files?dir=%2F2026-11-03&filter=RadioMan261103_183319.wav")


if __name__ == "__main__":
    unittest.main()