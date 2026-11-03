import unittest
from pathlib import Path
from unittest.mock import patch
import json

from scripts.test_support import validate_performance_snapshot
from scripts.preview_web import PreviewDevice
from scripts.test_wifi import recording_metadata_found, test_performance_switch


class PerformanceDiagnosticsTests(unittest.TestCase):
    def test_enabled_snapshot_requires_memory_tasks_nvs_and_audio_counters(self):
        snapshot = {key: 1 for key in (
            "uptimeMs", "loopMaxGapUs", "loopGapsOver100ms", "taskCount",
            "loopCurrentUs", "loopAverageUs", "loopPeakUs", "loopSamples",
            "displayCurrentUs", "displayAverageUs", "displayPeakUs", "displaySamples",
            "logBytes", "logSamples", "logSkippedBusy",
            "loopStackFreeMinimum", "recorderStackFreeMinimum", "speakerStackFreeMinimum",
            "lastFileWorkerStackFreeMinimum", "adcOverflows", "liveAudioDroppedFrames",
            "internalFree", "internalMinimumFree", "internalLargestBlock",
            "psramFree", "psramMinimumFree", "psramLargestBlock",
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

    def test_preview_sd_log_requires_opt_in_and_does_not_restart(self):
        device = PreviewDevice()
        self.assertEqual(device.api("GET", "/api/performance/log", {})[0], 409)
        self.assertEqual(device.api("POST", "/api/performance/log", {"enabled": "invalid"})[0], 400)
        started = device.api("POST", "/api/performance/log", {"enabled": "1"})[1]
        self.assertTrue(started["logEnabled"])
        self.assertTrue(started["logPath"].endswith(".txt"))
        self.assertEqual(device.api("GET", "/api/performance/log", {})[0], 200)
        self.assertFalse(device.api("POST", "/api/performance/log", {"enabled": "0"})[1]["logEnabled"])
        self.assertEqual(device.api("POST", "/api/performance", {"enabled": "0"})[1], {"enabled": False})
        self.assertEqual(device.api("POST", "/api/performance/log", {"enabled": "1"})[0], 409)
        self.assertFalse(PreviewDevice().performance_log_enabled)

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


if __name__ == "__main__":
    unittest.main()