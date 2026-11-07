import os
import shutil
import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]


class ReplayScheduleTests(unittest.TestCase):
    def test_replay_page_and_file_manager_selection_are_inert_until_saved(self):
        from scripts.web_assets import PAGES, render_page
        self.assertEqual(PAGES["automatic_replay"], "kAutomaticReplayPage")
        page = render_page("automatic_replay")
        self.assertIn('id="replayStop"', page)
        self.assertIn("/api/automatic-replay/rules", page)
        self.assertIn("Confirm on the encoder within 30 seconds", page)
        files = (ROOT / "web/file_actions.js").read_text(encoding="utf-8")
        self.assertIn('replayLink.href = "/automatic-replay?path="', files)
        self.assertIn('paths.append("path", item.path)', files)
        self.assertNotIn("/api/automatic-replay/arm", files)

    def test_preview_rules_stay_disarmed_and_stop_cancels_arm_request(self):
        from scripts.preview_web import PreviewDevice
        device = PreviewDevice()
        rule = {"path": "/REC00001.wav", "anchorUtc": 1794000000, "intervalSeconds": 0,
                "jitterMinutes": 0, "window": 0, "startMinute": 0, "endMinute": 0}
        self.assertEqual(device.api("POST", "/api/automatic-replay/rules", rule)[0], 200)
        self.assertFalse(device.api("GET", "/api/automatic-replay", {})[1]["armed"])
        self.assertFalse(device.api("GET", "/api/automatic-replay", {})[1]["rules"][0]["active"])
        self.assertEqual(device.api("POST", "/api/automatic-replay/arm", {"confirm": "MEASURED TOT"})[0], 409)
        self.assertEqual(device.api("POST", "/api/automatic-replay/activate", {"id": 1, "enabled": 1})[0], 200)
        self.assertEqual(device.api("POST", "/api/automatic-replay/arm", {"confirm": "MEASURED TOT"})[0], 202)
        stopped = device.api("POST", "/api/automatic-replay/stop", {})[1]
        self.assertFalse(stopped["armPending"])
        self.assertFalse(stopped["armed"])

    def test_sd_rule_api_remains_disarmed_after_boot_and_web_writes(self):
        controller = (ROOT / "src/automatic_replay.cpp").read_text(encoding="utf-8")
        web = (ROOT / "src/web_server.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn("initAutomaticReplay(sdReady);", main)
        self.assertIn("replayArmed.store(false);", controller)
        self.assertIn('"/replay-rules.tmp"', controller)
        self.assertIn('"/replay-rules.bak"', controller)
        self.assertIn("readConfig(kPendingPath", controller)
        self.assertIn('error = "Duplicate rule field"', controller)
        self.assertIn("readConfig(kConfigPath, verifiedRules", controller)
        self.assertIn("if (!storageValid || !appSdReady()", controller)
        self.assertIn("check.read() == static_cast<uint8_t>(serialized[index])", controller)
        self.assertIn('cJSON_DeleteItemFromObjectCaseSensitive(normalized, "active")', controller)
        self.assertIn("fileManagerBusy() || radioProgrammingBusy()", controller)
        self.assertIn('server.on("/api/automatic-replay/rules", HTTP_POST', web)
        self.assertIn('server.on("/api/automatic-replay/activate", HTTP_POST', web)
        self.assertIn('server.on("/api/automatic-replay/stop", HTTP_POST', web)

    def test_auto_transmit_requires_local_confirm_and_fresh_adc_at_ptt(self):
        controller = (ROOT / "src/automatic_replay.cpp").read_text(encoding="utf-8")
        playback = (ROOT / "src/playback.cpp").read_text(encoding="utf-8")
        display = (ROOT / "src/display.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn("pollAutomaticReplay();", main)
        self.assertIn("automaticReplayConfirmArm();", display)
        self.assertIn("replayNextOccurrence(", controller)
        self.assertIn("queueCount == kQueueLimit", controller)
        self.assertIn("popEvent(false);", controller)
        self.assertIn("quietSince.store(replaySilenceSince(", controller)
        self.assertIn("now - lastSampleAt.load()) <= 300", controller)
        self.assertIn("automaticPlayback.load() && !keyedAt && !automaticReplayReadyToKey()", playback)
        self.assertLess(playback.index("automaticReplayReadyToKey()"), playback.index("acquirePlaybackPtt(", playback.index("static bool startRadioSegment")))

    def test_native_schedule_and_windows(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("Host g++ required for replay schedule tests")
        source = r'''
            #include <cassert>
            #include "replay_rules.h"
            int main() {
              ReplaySchedule once;
              once.anchorUtc = 1760000000;
              assert(replayScheduleValid(once));
              assert(replayNextOccurrence(once, once.anchorUtc - 1, 1) == 0);
              assert(replayNextOccurrence(once, once.anchorUtc, 1) == UINT32_MAX);
              assert(replayDueAt(once, 0, 1) == once.anchorUtc);
              ReplaySchedule regular;
              regular.anchorUtc = 1760000000;
              regular.intervalSeconds = 3600;
              regular.jitterMinutes = 10;
              assert(replayScheduleValid(regular));
              for (uint32_t occurrence = 1; occurrence < 50; ++occurrence) {
                auto due = replayDueAt(regular, occurrence, 9);
                assert(due >= regular.anchorUtc + 3600 * occurrence - 600);
                assert(due <= regular.anchorUtc + 3600 * occurrence + 600);
                assert(due == replayDueAt(regular, occurrence, 9));
                assert(replayNextOccurrence(regular, due - 1, 9) <= occurrence);
                assert(replayNextOccurrence(regular, due, 9) > occurrence);
              }
              regular.jitterMinutes = 60;
              assert(!replayScheduleValid(regular));
              regular.jitterMinutes = 31;
              assert(!replayScheduleValid(regular));
              regular.jitterMinutes = 0;
              regular.window = ReplayWindow::active;
              regular.startMinute = 22 * 60;
              regular.endMinute = 6 * 60;
              assert(replayWindowContains(23 * 60, regular));
              assert(replayWindowContains(5 * 60, regular));
              assert(!replayWindowContains(12 * 60, regular));
              regular.window = ReplayWindow::quiet;
              assert(!replayWindowContains(23 * 60, regular));
              assert(replayWindowContains(12 * 60, regular));
                            uint32_t quiet = replaySilenceSince(0, 0, 0, 1000, 1, 20, 100);
                            assert(quiet == 1000);
                            for (uint32_t sequence = 2; sequence <= 101; ++sequence)
                                quiet = replaySilenceSince(quiet, 1000 + (sequence - 2) * 100, sequence - 1,
                                                                                     1000 + (sequence - 1) * 100, sequence, 20, 100);
                            assert(quiet == 1000);
                            assert(replaySilenceSince(quiet, 10900, 101, 11000, 102, 120, 100) == 0);
                            assert(replaySilenceSince(quiet, 10900, 101, 11350, 102, 20, 100) == 11350);
                            assert(replaySilenceSince(quiet, 10900, 101, 11000, 104, 20, 100) == 11000);
            }
        '''
        with TemporaryDirectory() as folder:
            program = Path(folder) / "replay.cpp"
            program.write_text(source, encoding="utf-8")
            executable = Path(folder) / "replay.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                                       str(program), "-o", str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)