import os
import copy
import io
import http.client
import json
import threading
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import unittest
import wave
from unittest.mock import patch

from scripts.preview_web import PreviewDevice, PreviewServer
from scripts.preview_auto import DEFAULTS, MODES
from scripts.test_device import test_automatic_settings
from scripts.web_assets import load_languages
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


class AutoRecordRulesTests(unittest.TestCase):
    def test_http_automatic_settings_validation_and_safe_arming(self):
        server = PreviewServer(("127.0.0.1", 0))
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def request(method, path, body=None):
            client = http.client.HTTPConnection(*server.server_address, timeout=5)
            client.request(method, path, json.dumps(body) if body is not None else None, {"Content-Type": "application/json"})
            response = client.getresponse()
            status, data = response.status, response.read()
            client.close()
            return status, json.loads(data) if status == 200 else data

        settings = copy.deepcopy(DEFAULTS)
        settings["radio"]["threshold"] = 155
        self.assertEqual(request("POST", "/api/auto-record/settings", settings)[0], 200)
        invalid = copy.deepcopy(settings)
        invalid["priorities"] = ["autoRadio"] * 4
        self.assertEqual(request("POST", "/api/auto-record/settings", invalid)[0], 400)
        state = request("GET", "/api/auto-record")[1]
        self.assertEqual(state["radio"]["threshold"], 155)
        self.assertFalse(state["radio"]["armed"])
        self.assertEqual(request("POST", "/api/auto-record/arm?source=radio&enabled=1")[0], 200)
        self.assertTrue(request("GET", "/api/auto-record")[1]["radio"]["armed"])
        self.assertEqual(request("GET", "/api/auto-record")[1]["activeMode"], "none")
        self.assertEqual(request("POST", "/api/auto-record/arm?source=radio&enabled=0")[0], 200)
        self.assertEqual(request("POST", "/api/auto-record/arm?source=invalid&enabled=1")[0], 400)
        self.assertIn("/automatic.js", (ROOT / "web/settings.html").read_text(encoding="utf-8"))
        self.assertEqual(request("POST", "/api/auto-record/settings", [1, 2])[0], 400)

    def test_translated_oled_priority_and_automatic_status_fit(self):
        for code, catalog in load_languages().items():
            values = [catalog["oled"][label] for label in ("Manual mic", "Manual radio", "Auto radio", "Auto mic")]
            for rank in range(1, 5):
                with self.subTest(language=code, rank=rank):
                    label = catalog["oled"]["Priority " + str(rank)]
                    self.assertLessEqual(9 + len(label) * 5 + 2, 127 - max(map(len, values)) * 5)
            self.assertLessEqual(len(catalog["oled"]["RMS:%u Quiet:%us"] % (32768, 60)) * 5, 128)
            self.assertLessEqual(max(len(catalog["oled"][key]) for key in ("Auto: ON", "Auto: OFF")) * 5 + 2,
                                 128 - len("RMS:32768") * 5)
        display = (ROOT / "src/display.cpp").read_text(encoding="utf-8")
        automatic = display.split("void drawAutomatic(bool microphone)", 1)[1].split("void drawBuildDate", 1)[0]
        self.assertIn('"RMS:%u", status.rms', automatic)
        self.assertIn('drawText(128 - oled.getStrWidth(rms), 31, rms)', automatic)
        self.assertIn('"RMS:--"', automatic)

    def test_recorder_preserves_prebuffer_during_priority_handoff(self):
        source = (ROOT / "src/recorder.cpp").read_text(encoding="utf-8")
        idle_monitor = source.split("void monitorAutomaticIdle()", 1)[1].split("void monitorOtherAutomatic", 1)[0]
        self.assertNotIn("History.clear()", idle_monitor)
        self.assertIn("micHistory.push(pcmBuffer, pcmLength)", source)
        self.assertIn("radioHistory.push(pcmBuffer, pcmLength)", source)
        capture = source.split("void recordLoop", 1)[1]
        self.assertLess(capture.index(".write(file, dataBytes)"), capture.index("i2s_channel_enable(micHandle)"))
        self.assertIn("valid = started && writeWavHeader", capture)
        self.assertIn("while (!automaticAnyArmed() && (signalRequested()", source)
        self.assertIn("while (!automaticAnyArmed() && micSignalRequested()", source)
        manager = (ROOT / "src/file_manager.cpp").read_text(encoding="utf-8")
        listing = manager.split("bool fileManagerList(", 1)[1].split("bool fileManagerListOffset", 1)[0]
        self.assertIn("pollAutomaticRecording();", listing)
        self.assertIn('page.error = "SD unavailable or recording in progress"', listing)
        serial = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        command = serial.split("static void handleSerialCommand", 1)[1]
        self.assertLess(command.index("if (receivingRadioPlaybackPath)"), command.index("if (command == 'a')"))
        self.assertLess(command.index("if (receivingDeleteFilename)"), command.index("if (command == 'a')"))

    def test_native_threshold_silence_freshness_and_priority_rules(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for automatic recording rules")
        harness = r'''
#include <cassert>
#include <vector>
#include "auto_record_rules.h"
#include "pcm_history.h"
struct Output {
    std::vector<uint8_t> bytes;
    size_t write(const uint8_t *data,size_t length) { bytes.insert(bytes.end(),data,data+length); return length; }
};
int main() {
  AutoRecordConfig config;
  assert(config.threshold==100 && config.silenceSeconds==3);
  assert(validAutoConfig(config,false));
  config.threshold=4096; assert(!validAutoConfig(config,false)); assert(validAutoConfig(config,true));
  config.threshold=100; config.silenceSeconds=61; assert(!validAutoConfig(config,true)); config.silenceSeconds=3;
  AutoDetector detector; detector.arm(true,0);
  detector.observe(200,1000,config,false); assert(!detector.ready(1000));
  detector.observe(100,2000,config,false); assert(!detector.ready(2000));
  detector.observe(100,2100,config,false); assert(detector.ready(2100));
  assert(!detector.ready(3000));
  detector.observe(80,2200,config,true); assert(!detector.silent(5199,config)); assert(detector.silent(5200,config));
  detector.arm(false,5300); assert(!detector.ready(5300)); assert(detector.silent(5300,config));
  detector.arm(true,0x100000000ULL); detector.observe(100,0x100000800ULL,config,false);
  detector.observe(100,0x100000864ULL,config,false); assert(detector.ready(0x100000864ULL));
  std::array<RecordingMode,4> priorities={RecordingMode::manualMic,RecordingMode::manualRadio,RecordingMode::autoRadio,RecordingMode::autoMic};
  assert(validRecordingPriorities(priorities));
  std::array<bool,4> requested={false,false,true,true};
  assert(chooseRecordingMode(priorities,requested)==RecordingMode::autoRadio);
  requested[0]=true; assert(chooseRecordingMode(priorities,requested)==RecordingMode::manualMic);
  priorities[3]=RecordingMode::manualMic; assert(!validRecordingPriorities(priorities));
    PcmHistory<8> history;
    uint8_t samples[]={0,1,2,3,4,5,6,7,8,9}; history.push(samples,10);
    Output output; uint32_t written=0;
    assert(history.write(output,written)); assert(written==8);
    assert(output.bytes==std::vector<uint8_t>({2,3,4,5,6,7,8,9}));
    history.clear(); assert(history.count==0);
}
'''
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / "rules.cpp").write_text(harness, encoding="utf-8")
            executable = directory / ("rules.exe" if os.name == "nt" else "rules")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                                       str(directory / "rules.cpp"), "-o", str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preview_auto_silence_and_pcm_duration_for_both_sources(self):
        for source, mode, prefix in (("radio", "autoRadio", "RadioAut"), ("microphone", "autoMic", "MicAut")):
            with self.subTest(source=source), patch("scripts.preview_web.time.monotonic", return_value=0):
                device = PreviewDevice()
                device.automatic.rms_override = {"radio": 0, "microphone": 0}
                device.automatic.rms_override[source] = 100
                self.assertEqual(device.api("POST", "/api/auto-record/arm", {"source": source, "enabled": "1"})[0], 200)
                with patch("scripts.preview_web.time.monotonic", return_value=2.1):
                    self.assertEqual(device.api("GET", "/api/auto-record", {})[1]["activeMode"], mode)
                    path = device.recording_name
                    self.assertIn(prefix, path)
                device.automatic.rms_override[source] = 79
                with patch("scripts.preview_web.time.monotonic", return_value=5.0):
                    self.assertTrue(device.api("GET", "/api/auto-record", {})[1][source]["recording"])
                with patch("scripts.preview_web.time.monotonic", return_value=5.1):
                    status = device.api("GET", "/api/auto-record", {})[1]
                    self.assertEqual(status["activeMode"], "none")
                    self.assertTrue(status[source]["armed"])
                    self.assertEqual(status[source]["lastFile"], path)
                    self.assertEqual(status[source]["lastElapsedMs"], 3500)
                    with wave.open(io.BytesIO(device.files[path]), "rb") as recording:
                        self.assertEqual(recording.getnframes(), 22050 * 3.5)

    def test_preview_manual_priority_preempts_without_parallel_files(self):
        with patch("scripts.preview_web.time.monotonic", return_value=0):
            device = PreviewDevice()
            device.automatic.rms_override = {"radio": 200, "microphone": 0}
            device.api("POST", "/api/auto-record/arm", {"source": "radio", "enabled": "1"})
        with patch("scripts.preview_web.time.monotonic", return_value=2.1):
            self.assertEqual(device.api("GET", "/api/auto-record", {})[1]["activeMode"], "autoRadio")
            old_path = device.recording_name
        with patch("scripts.preview_web.time.monotonic", return_value=2.2):
            self.assertEqual(device.api("POST", "/api/mic/start", {})[0], 200)
            self.assertEqual(device.automatic.mode, "manualMic")
            self.assertIn(old_path, device.files)
            self.assertNotEqual(old_path, device.recording_name)
            self.assertEqual(device.api("POST", "/api/start", {})[0], 409)
        with patch("scripts.preview_web.time.monotonic", return_value=3.2):
            self.assertEqual(device.api("POST", "/api/stop", {})[0], 200)
            self.assertFalse(device.recording)
            self.assertTrue(device.automatic.sources["radio"]["armed"])

    def test_preview_auto_settings_validate_and_restore_without_arming(self):
        device = PreviewDevice()
        for invalid in ({**copy.deepcopy(DEFAULTS), "priorities": ["manualMic"] * 4},
                        {**copy.deepcopy(DEFAULTS), "priorities": [0, "manualRadio", "autoRadio", "autoMic"]},
                        {**copy.deepcopy(DEFAULTS), "radio": {"threshold": 4096, "silenceSeconds": 3}},
                        {**copy.deepcopy(DEFAULTS), "microphone": {"threshold": 100, "silenceSeconds": 61}}):
            self.assertEqual(device.api("POST", "/api/auto-record/settings", invalid)[0], 400)
            self.assertEqual(device.automatic.settings, DEFAULTS)
        saved = copy.deepcopy(DEFAULTS)
        saved["radio"] = {"threshold": 155, "silenceSeconds": 5}
        saved["microphone"] = {"threshold": 220, "silenceSeconds": 2}
        saved["priorities"] = list(reversed(saved["priorities"]))
        self.assertEqual(device.api("POST", "/api/auto-record/settings", saved)[0], 200)
        with_extra = copy.deepcopy(saved)
        with_extra["radio"]["armed"] = True
        self.assertEqual(device.api("POST", "/api/auto-record/settings", with_extra)[0], 200)
        self.assertFalse(device.api("GET", "/api/auto-record", {})[1]["radio"]["armed"])
        restored = PreviewDevice(saved_auto=saved)
        self.assertEqual(restored.automatic.settings, saved)
        self.assertFalse(restored.automatic.sources["radio"]["armed"])
        self.assertFalse(restored.automatic.sources["microphone"]["armed"])

    def test_serial_automatic_configuration_probe_restores_all_values(self):
        config = copy.deepcopy(DEFAULTS)
        config.update(activeMode="none", activeFile="")
        for source in ("radio", "microphone"):
            config[source].update(armed=False, lastFile="")
        navigation = {"tile": 0, "row": 0, "settings": False, "editing": False}
        draft = copy.deepcopy(config)

        def write(command):
            nonlocal draft
            if command in (b"j", b"l"):
                direction = 1 if command == b"l" else -1
                if navigation["editing"]:
                    row = navigation["row"]
                    if row <= 18:
                        source = "microphone" if row >= 17 else "radio"
                        field = "threshold" if row in (15, 17) else "silenceSeconds"
                        draft[source][field] += direction * (10 if field == "threshold" else 1)
                    else:
                        rank = row - 19
                        value = MODES[(MODES.index(draft["priorities"][rank]) + direction) % 4]
                        other = draft["priorities"].index(value)
                        draft["priorities"][rank], draft["priorities"][other] = draft["priorities"][other], draft["priorities"][rank]
                elif navigation["settings"]:
                    navigation["row"] = (navigation["row"] + direction) % 24
                else:
                    navigation["tile"] = (navigation["tile"] + direction) % 12
            elif command == b"b":
                if navigation["editing"]: navigation["editing"] = False
                else: navigation["settings"] = False
            elif command == b"k":
                if navigation["editing"]:
                    config.update(copy.deepcopy(draft)); navigation["editing"] = False
                elif navigation["settings"]:
                    draft = copy.deepcopy(config); navigation["editing"] = True
                elif navigation["tile"] == 11:
                    navigation["settings"] = True; navigation["row"] = 0
                elif navigation["tile"] in (2, 3):
                    source = "microphone" if navigation["tile"] == 3 else "radio"
                    config[source]["armed"] = not config[source]["armed"]

        with patch("scripts.test_device.menu_state", side_effect=lambda device:
                   (navigation["tile"], navigation["row"], int(navigation["settings"]), int(navigation["editing"]), 0)), \
                patch("scripts.test_device.automatic_snapshot", side_effect=lambda device, report: copy.deepcopy(config)):
            self.assertIn("settings/navigation restored", test_automatic_settings(SimpleNamespace(write=write), None))
        self.assertEqual(config["priorities"], list(MODES))
        for source in ("radio", "microphone"):
            self.assertEqual((config[source]["threshold"], config[source]["silenceSeconds"], config[source]["armed"]), (100, 3, False))
        self.assertEqual((navigation["tile"], navigation["settings"]), (0, False))

    def test_actual_coordinator_preemption_and_atomic_persistence(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for actual recording coordinator")
        stubs = {
            "Arduino.h": "#pragma once\n#include <string>\n#include <cstdint>\nusing String=std::string; using SemaphoreHandle_t=void*; constexpr uint32_t portMAX_DELAY=0xffffffff; inline void *xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);} inline void xSemaphoreTake(void*,uint32_t){} inline void xSemaphoreGive(void*){}\n",
            "recorder.h": "#pragma once\n#include <cstdint>\nenum class CaptureSource{adc,microphone}; enum class CaptureState{idle,starting,recording}; bool recorderMicReady(); bool recorderAdcReady(); bool recorderTaskReady(); CaptureState recorderState(); const char *recorderName(); const char *recorderError(); uint64_t recorderElapsedMs(); bool recorderFinalized(); bool recorderStart(bool,CaptureSource,bool); bool stopRecording(); void recorderNotify();\n",
            "app_control.h": "#pragma once\nbool appSdReady();\n",
            "file_manager.h": "#pragma once\nbool fileManagerBusy();\n",
            "live_audio.h": "#pragma once\nint liveAudioClient();\n",
            "playback.h": "#pragma once\nenum class PlaybackState{idle,starting,playing}; PlaybackState playbackGetState();\n",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nextern uint64_t clockMs; inline int64_t esp_timer_get_time(){return clockMs*1000;}\n",
            "Preferences.h": "#pragma once\n#include <vector>\n#include <cstring>\nextern std::vector<unsigned char> stored; extern bool failWrite; extern int writes; struct Preferences{bool begin(const char*,bool){return true;} size_t getBytesLength(const char*){return stored.size();} size_t getBytes(const char*,void *value,size_t size){memcpy(value,stored.data(),size);return size;} size_t putBytes(const char*,const void *value,size_t size){++writes;if(failWrite)return 0;stored.assign(static_cast<const unsigned char*>(value),static_cast<const unsigned char*>(value)+size);return size;} void end(){}};\n",
            "cJSON.h": "#pragma once\nstruct cJSON{}; inline cJSON *cJSON_CreateObject(){return nullptr;} template<class... Args> void cJSON_AddStringToObject(Args...){} template<class... Args> void cJSON_AddNumberToObject(Args...){} template<class... Args> void cJSON_AddBoolToObject(Args...){} template<class... Args> cJSON *cJSON_AddObjectToObject(Args...){return nullptr;} template<class... Args> cJSON *cJSON_AddArrayToObject(Args...){return nullptr;} inline cJSON *cJSON_CreateString(const char*){return nullptr;} inline void cJSON_AddItemToArray(cJSON*,cJSON*){} inline char *cJSON_PrintUnformatted(cJSON*){return nullptr;} inline void cJSON_free(void*){} inline void cJSON_Delete(cJSON*){}\n",
        }
        harness = r'''
#include <cassert>
#include <vector>
#include "automatic_record.h"
#include "playback.h"
uint64_t clockMs=0; std::vector<unsigned char> stored; bool failWrite=false; int writes=0;
CaptureState state=CaptureState::idle; bool stopped=false, finalized=false; int starts=0;
const char *name="", *error=""; uint64_t elapsed=0;
bool appSdReady(){return true;} bool fileManagerBusy(){return false;} int liveAudioClient(){return -1;}
PlaybackState playbackGetState(){return PlaybackState::idle;}
bool recorderAdcReady(){return true;} bool recorderMicReady(){return true;} bool recorderTaskReady(){return true;}
CaptureState recorderState(){return state;} const char *recorderName(){return name;} const char *recorderError(){return error;}
uint64_t recorderElapsedMs(){return elapsed;} bool recorderFinalized(){return finalized;} void recorderNotify(){}
bool stopRecording(){stopped=true;return state!=CaptureState::idle;}
bool recorderStart(bool,CaptureSource source,bool automatic){assert(state==CaptureState::idle);++starts;stopped=finalized=false;state=CaptureState::recording;name=automatic?(source==CaptureSource::adc?"RadioAut.wav":"MicAut.wav"):(source==CaptureSource::adc?"RadioMan.wav":"MicMan.wav");return true;}
void finish(){state=CaptureState::idle;finalized=true;elapsed=3500;pollAutomaticRecording();}
int main(){
 assert(initAutomaticRecording()); assert(!automaticAnyArmed());
 assert(automaticSetArmed(false,true)); clockMs=2000;automaticObserve(false,100);pollAutomaticRecording();assert(starts==0);
 clockMs=2100;automaticObserve(false,100);pollAutomaticRecording();assert(starts==1);assert(automaticCurrentMode()==RecordingMode::autoRadio);
 assert(automaticRequestManual(CaptureSource::microphone));assert(stopped);assert(starts==1);
 assert(!automaticRequestManual(CaptureSource::adc));assert(starts==1);
 finish();assert(starts==2);assert(automaticCurrentMode()==RecordingMode::manualMic);assert(automaticStatus(false).lastFile=="RadioAut.wav");
 assert(!automaticRequestManual(CaptureSource::adc));assert(automaticStopCurrent());finish();assert(automaticCurrentMode()==RecordingMode::none);
 clockMs=3500;automaticObserve(false,100);clockMs=3600;automaticObserve(false,100);pollAutomaticRecording();assert(starts==3);
 clockMs=6500;automaticObserve(false,0);pollAutomaticRecording();assert(!stopped);
 clockMs=6600;automaticObserve(false,0);pollAutomaticRecording();assert(stopped);finish();assert(automaticStatus(false).lastElapsedMs==3500);
 automaticSetArmed(false,false);
 AutoRecordConfig radio;radio.threshold=155;radio.silenceSeconds=5;AutoRecordConfig mic;mic.threshold=220;mic.silenceSeconds=2;
 std::array<RecordingMode,4> order={RecordingMode::autoMic,RecordingMode::autoRadio,RecordingMode::manualRadio,RecordingMode::manualMic};
 assert(automaticSaveSettings(radio,mic,order));assert(writes==1);assert(automaticSaveSettings(radio,mic,order));assert(writes==1);
 failWrite=true;radio.threshold=156;assert(!automaticSaveSettings(radio,mic,order));assert(automaticStatus(false).config.threshold==155);failWrite=false;
 assert(initAutomaticRecording());assert(automaticStatus(false).config.threshold==155);assert(!automaticAnyArmed());
 clockMs=7000;assert(automaticRequestManual(CaptureSource::microphone));assert(automaticSetArmed(true,true));
 clockMs=9000;automaticObserve(true,300);clockMs=9100;automaticObserve(true,300);pollAutomaticRecording();assert(stopped);
 int before=starts;finish();assert(starts==before+1);assert(automaticCurrentMode()==RecordingMode::autoMic);
 assert(!automaticRequestManual(CaptureSource::adc));state=CaptureState::idle;finalized=false;error="SD write failed";pollAutomaticRecording();
 assert(!automaticStatus(true).armed);assert(automaticStatus(true).error=="SD write failed");
 error="";clockMs=10000;assert(automaticSetArmed(false,true));clockMs=16001;pollAutomaticRecording();
 assert(!automaticStatus(false).armed);assert(automaticStatus(false).error=="Audio input did not produce samples");
 clockMs=17000;assert(automaticSetArmed(true,true));automaticObserve(true,0);clockMs=19501;pollAutomaticRecording();
 assert(!automaticStatus(true).armed);assert(automaticStatus(true).error=="Audio signal timed out");
 for (bool microphone : {false,true}) {
     clockMs+=1000;assert(automaticSetArmed(microphone,true));
     clockMs+=2000;automaticObserve(microphone,300);clockMs+=100;automaticObserve(microphone,300);pollAutomaticRecording();
     assert(automaticCurrentMode()==(microphone?RecordingMode::autoMic:RecordingMode::autoRadio));
     for(int window=0;window<100;++window){clockMs+=100;automaticObserve(microphone,0);pollAutomaticRecording();}
     assert(stopped);finish();
     assert(automaticStatus(microphone).armed);assert(automaticStatus(microphone).error.empty());
     assert(automaticStatus(microphone).lastFile==(microphone?"MicAut.wav":"RadioAut.wav"));
     int previousStarts=starts;
     clockMs+=100;automaticObserve(microphone,0);pollAutomaticRecording();assert(automaticStatus(microphone).armed);
     clockMs+=2000;automaticObserve(microphone,300);clockMs+=100;automaticObserve(microphone,300);pollAutomaticRecording();
     assert(starts==previousStarts+1);
     automaticSetArmed(microphone,false);finish();
 }
}
'''
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as folder:
            directory = Path(folder)
            for name, content in stubs.items():
                (directory / name).write_text(content, encoding="utf-8")
            (directory / "automatic_record.h").write_text((ROOT / "include/automatic_record.h").read_text(encoding="utf-8"), encoding="utf-8")
            (directory / "test.cpp").write_text(harness, encoding="utf-8")
            executable = directory / ("test.exe" if os.name == "nt" else "test")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory), "-I", str(ROOT / "include"),
                                       str(directory / "test.cpp"), str(ROOT / "src/automatic_record.cpp"), "-o", str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)