import http.client
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
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
    def test_settings_runtime_advances_and_handles_rollover(self):
        self.assertIn('id="runtime"', render_page("settings"))
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js required for runtime regression")
        source = (WEB_ROOT / "settings.js").read_text(encoding="utf-8")
        runtime_script = "function formatUptime" + source.split("function formatUptime", 1)[1].split("async function loadClock", 1)[0]
        script = """
const assert = require('node:assert/strict');
let now = 0;
const performance = {now: () => now};
const output = {textContent: ''};
const document = {querySelector: () => output};
const clockZone = {}, clockCurrent = {}, clockStatus = {}, clockLocal = {};
const setInterval = () => {};
""" + runtime_script + """
for (const [seconds, expected] of [[0,'00:00:00 00:00:00'],[86399,'00:00:00 23:59:59'],
                                 [86400,'00:00:01 00:00:00'],[30*86400,'00:01:00 00:00:00'],
                                 [360*86400,'01:00:00 00:00:00'],[4294968,'00:01:19 17:02:48']]) {
  assert.equal(formatUptime(seconds), expected);
}
showClock({uptimeSeconds:59, zone:0, source:'rtc',local:'2026-11-01T16:21:28'});
assert.equal(output.textContent, '00:00:00 00:00:59');
now += 1000;
renderUptime();
assert.equal(output.textContent, '00:00:00 00:01:00');
showClock({uptimeSeconds:60, zone:0, source:'manual',local:'2030-01-01T00:00:00'});
assert.equal(output.textContent, '00:00:00 00:01:00');
showClock({uptimeSeconds:0, zone:0, source:'rtc'});
assert.equal(output.textContent, '00:00:00 00:00:00');
showClock({zone:0, source:'unset'});
assert.equal(output.textContent, 'Unavailable');
"""
        result = subprocess.run([node, "-"], input=script, text=True, encoding="utf-8", capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preview_runtime_is_independent_of_clock_changes(self):
        with patch("scripts.preview_web.time.monotonic", return_value=100):
            device = PreviewDevice("ready")
        with patch("scripts.preview_web.time.monotonic", return_value=100 + 50 * 86400):
            self.assertEqual(device.clock_info()["uptimeSeconds"], 50 * 86400)
            device.clock_epoch = 1793550088
            device.clock_source = "manual"
            device.clock_started_at = 100 + 50 * 86400
            self.assertEqual(device.clock_info()["uptimeSeconds"], 50 * 86400)

    def test_home_header_contains_device_date_and_time(self):
        page = render_page("index")
        header = page.split("<header>", 1)[1].split("</header>", 1)[0]
        self.assertIn('id="demoClock"', header)
        self.assertEqual(page.count('id="demoClock"'), 1)
        script = (WEB_ROOT / "index.js").read_text(encoding="utf-8")
        self.assertIn('Date.parse(data.clock.local + "Z")', script)
        self.assertNotIn('demoBase = data.demoSeconds', script)

    def test_header_clock_rolls_over_dates_without_pc_timezone_conversion(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is required for the browser clock regression")
        source = (WEB_ROOT / "index.js").read_text(encoding="utf-8")
        clock_script = "let clockBase" + source.split("let clockBase", 1)[1].split("const signalHistory", 1)[0]
        script = """
const assert = require('node:assert/strict');
process.env.TZ = 'America/Los_Angeles';
let now = 0;
const performance = { now: () => now };
const element = { textContent: '', dateTime: '', removeAttribute: () => { element.dateTime = ''; } };
const document = { querySelector: () => element };
const setInterval = () => {};
""" + clock_script + """
for (const [source, prefix] of Object.entries({internet:'NTP', manual:'MANUAL', rtc:'RTC', saved:'SAVED', default:'DEFAULT'})) {
  clockSource = source;
  clockBase = Date.parse('2026-11-01T16:10:02Z');
  demoSyncedAt = now;
  renderDemoClock();
  assert.equal(element.textContent, prefix + ' 2026.11.01. 16:10:02');
  assert.equal(element.dateTime, '2026-11-01T16:10:02');
}
for (const [start, expected] of [['2026-12-31T23:59:59','2027.01.01. 00:00:00'],
                               ['2028-02-28T23:59:59','2028.02.29. 00:00:00']]) {
  clockSource = 'rtc';
  clockBase = Date.parse(start + 'Z');
  demoSyncedAt = now;
  now += 1000;
  renderDemoClock();
  assert.equal(element.textContent, 'RTC ' + expected);
}
clockSource = 'unset';
renderDemoClock();
assert.equal(element.textContent, 'TIME UNSET');
assert.equal(element.dateTime, '');
clockSource = 'rtc';
clockBase = NaN;
renderDemoClock();
assert.equal(element.textContent, 'TIME UNSET');
"""
        result = subprocess.run([node, "-"], input=script, text=True, encoding="utf-8",
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_dac_tone_uses_independent_output_and_timed_web_control(self):
        root = WEB_ROOT.parent
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        tone = playback.split('static void playDacTone()', 1)[1].split('static void speakerLoop(', 1)[0]
        self.assertIn('I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER)', playback)
        self.assertIn('startDacOutput(outputHandle, config)', tone)
        self.assertIn('i2s_new_channel(&channel, &outputHandle, nullptr)', playback)
        self.assertIn('gpio_reset_pin(kSpeakerDataGpio)', playback)
        self.assertIn('gpio_set_direction(kSpeakerDataGpio, GPIO_MODE_INPUT)', playback)
        self.assertLess(playback.index('i2s_del_channel(speakerHandle)', playback.index('static esp_err_t startDacOutput(')),
            playback.index('i2s_new_channel(&channel, &outputHandle, nullptr)', playback.index('static esp_err_t startDacOutput(')))
        self.assertIn('speakerReady = initSpeaker()', playback)
        self.assertIn('i2s_channel_write(outputHandle,', tone)
        self.assertNotIn('i2s_channel_write(speakerHandle,', tone)
        self.assertNotIn('recorderStartRadioOutput()', tone)
        self.assertIn('kToneDurationSeconds = 10', playback)
        self.assertIn('playbackStartTone(', playback)
        self.assertIn('playbackStopRequested.load()', playback)
        self.assertIn('server.on("/api/dac-tone", HTTP_POST', server)
        self.assertIn('server.on("/dac-test", HTTP_GET', server)
        self.assertIn('kDacTestPage', PAGES.values())

    def test_manual_dac_tone_remains_without_automatic_demo(self):
        root = WEB_ROOT.parent
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        page = render_page("dac_test")
        self.assertNotIn('playbackStartDacDemo', playback)
        self.assertNotIn('dacDemoStep', server)
        self.assertNotIn('server.on("/api/dac-demo", HTTP_POST', server)
        self.assertNotIn('id="startDemo"', page)
        script = (WEB_ROOT / "dac_test.js").read_text(encoding="utf-8")
        self.assertNotIn("request('/api/dac-demo')", script)
        self.assertIn("request('/api/dac-tone'", script)
        self.assertIn("request('/api/play/stop')", script)

    def test_radio_page_is_manual_read_only_and_embedded(self):
        server = (WEB_ROOT.parent / "src" / "web_server.cpp").read_text(encoding="utf-8")
        script = (WEB_ROOT / "radio.js").read_text(encoding="utf-8")
        self.assertIn('server.on("/radio", HTTP_GET', server)
        self.assertIn('server.on("/api/radio/read", HTTP_POST', server)
        self.assertIn('server.arg("confirm") != "1"', server)
        self.assertIn('server.on("/api/radio/image", HTTP_GET', server)
        self.assertIn('server.on("/api/radio/write", HTTP_POST', server)
        self.assertIn('server.arg("confirm") != "WRITE UV5R"', server)
        self.assertIn('if (busy || !wiring.checked) return;', script)
        self.assertNotIn('innerHTML', script)
        self.assertIn('kRadioPage', PAGES.values())
        self.assertIn('GPIO17', render_page("radio"))

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
        self.assertIn('if (!confirm(prompt))', files_script)
        self.assertIn('radio.dataset.radioPlay = item.path', files_script)

    def test_radio_dac_has_separate_playback_target_from_microphone_channel(self):
        root = WEB_ROOT.parent
        recorder = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        self.assertIn('i2s_new_channel(&channel, nullptr, &micHandle)', recorder)
        self.assertNotIn('radioHandle', recorder)
        self.assertIn('I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER)', playback)
        self.assertIn('PlaybackTarget::radio', playback)
        self.assertIn('cuePlaying.load() ? PlaybackTarget::speaker : playbackTarget', playback)
        self.assertIn('playbackStart(selectedFile, PlaybackTarget::radio)', display)
        self.assertIn('esp_err_t result = i2s_channel_write(outputHandle,', playback)
        self.assertIn('float requestedRadioGain = radioVolume.load() * radioBoost.load() / 100.0f', playback)
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        self.assertIn('targetArg == "radio" ? PlaybackTarget::radio', server)
        self.assertIn('data.radioReady', (root / "web" / "files.js").read_text(encoding="utf-8"))

    def test_radio_wav_uses_independent_44100_philips_output_with_original_progress(self):
        playback = (WEB_ROOT.parent / "src" / "playback.cpp").read_text(encoding="utf-8")
        wav_loop = playback.split('bool radio = playbackTarget == PlaybackTarget::radio;', 1)[1].split('static bool initSpeaker()', 1)[0]
        self.assertIn('I2S_STD_CLK_DEFAULT_CONFIG(44100)', playback)
        self.assertIn('I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT', playback)
        self.assertIn('startDacOutput(', wav_loop)
        self.assertIn('stopDacOutput(', wav_loop)
        self.assertNotIn('recorderStartRadioOutput()', wav_loop)
        self.assertIn('radioOutput[index * 4] =', wav_loop)
        self.assertIn('radioOutput[index * 4 + 1] =', wav_loop)
        self.assertIn('radioOutput[index * 4 + 2] =', wav_loop)
        self.assertIn('radioOutput[index * 4 + 3] =', wav_loop)
        self.assertIn('size_t bytes = count * (radio ? 4 : 2)', wav_loop)
        self.assertIn('playbackBytesPlayed.fetch_add(count)', wav_loop)
        self.assertIn('if (radio && !speakerReady) stopDacOutput(outputHandle, enabled)', wav_loop)

    def test_radio_volume_persists_and_file_player_shows_real_progress(self):
        root = WEB_ROOT.parent
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        page = (WEB_ROOT / "files.html").read_text(encoding="utf-8")
        script = (WEB_ROOT / "files.js").read_text(encoding="utf-8")
        self.assertIn('radioVolume{5}', playback)
        self.assertIn('putUChar(kRadioVolumeKey, radio)', playback)
        self.assertIn('getUChar(kRadioVolumeKey', playback)
        self.assertIn('playbackBytesPlayed.store', playback)
        self.assertIn('playbackBytesTotal.store', playback)
        self.assertIn('cuePlaying.load() ? 0 : playbackBytesPlayed.load()', playback)
        self.assertIn('\\"radioVolume\\"', server)
        self.assertIn('\\"playbackBytesPlayed\\"', server)
        self.assertIn('\\"playbackBytesTotal\\"', server)
        self.assertIn('server.on("/api/play/radio-volume"', server)
        self.assertIn('"Radio volume"', display)
        self.assertIn('playbackSetRadioVolume(', display)
        self.assertIn('id="radioVolume"', page)
        self.assertIn('id="playbackProgress"', page)
        self.assertIn('data.playbackBytesPlayed', script)

    def test_radio_wav_gain_is_independent_limited_and_configurable(self):
        root = WEB_ROOT.parent
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        page = (WEB_ROOT / "files.html").read_text(encoding="utf-8")
        script = (WEB_ROOT / "files.js").read_text(encoding="utf-8")
        wav_loop = playback.split('bool radio = playbackTarget == PlaybackTarget::radio;', 1)[1].split('static bool initSpeaker()', 1)[0]
        self.assertIn('radioBoost{1}', playback)
        self.assertIn('putUChar(kRadioBoostKey, radioGain)', playback)
        self.assertIn('getUChar(kRadioBoostKey', playback)
        self.assertIn('peak ? fminf(requestedRadioGain, maxOutput / peak)', wav_loop)
        self.assertIn('source * appliedRadioGain', wav_loop)
        self.assertIn('\\"radioBoost\\"', server)
        self.assertIn('server.on("/api/play/radio-boost", HTTP_POST', server)
        self.assertIn('value != "1" && value != "2" && value != "4" && value != "8" && value != "16"', server)
        self.assertIn('id="radioBoost"', page)
        self.assertIn('data.radioBoost', script)
        self.assertIn('playing && data.playbackTarget === "radio"', script)

        self.assertIn('/api/play/radio-volume?value=', script)

    def test_radio_dac_write_failure_reports_driver_result_and_progress(self):
        playback = (WEB_ROOT.parent / "src" / "playback.cpp").read_text(encoding="utf-8")
        self.assertIn('esp_err_t result = i2s_channel_write(outputHandle', playback)
        self.assertIn('esp_err_to_name(result)', playback)
        self.assertIn('written, bytes', playback)

    def test_adc_probe_reads_raw_adc1_without_changing_audio_capture(self):
        root = WEB_ROOT.parent
        recorder = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        page = render_page("test")
        self.assertIn('i2s_new_channel(&channel, nullptr, &micHandle)', recorder)
        self.assertIn('adc_continuous_io_to_channel(gpio, &unit, &channel)', recorder)
        self.assertIn('adc_continuous_config(adcHandle, &config)', recorder)
        self.assertIn('adc_continuous_config(adcHandle, &restore)', recorder)
        self.assertIn('server.on("/api/adc/raw", HTTP_GET', server)
        self.assertIn('id="adcPin"', page)
        self.assertIn('id="adcChart"', page)
        self.assertIn('id="pttToggle"', page)

    def test_radio_dac_and_microphone_use_separate_pins_and_serial_status(self):
        root = WEB_ROOT.parent
        recorder = (root / "src" / "recorder.cpp").read_text(encoding="utf-8")
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn('radioHandle', recorder)
        self.assertNotIn('recorderRadioHandle()', playback)
        self.assertIn('config.gpio_cfg.bclk = kDacBclkGpio;', playback)
        self.assertIn('config.gpio_cfg.ws = kDacLrcGpio;', playback)
        self.assertIn('config.gpio_cfg.dout = kDacDataGpio;', playback)
        self.assertIn('config.gpio_cfg.din = GPIO_NUM_8;', recorder)
        self.assertIn('config.gpio_cfg.bclk = GPIO_NUM_6;', recorder)
        self.assertIn('config.gpio_cfg.ws = GPIO_NUM_7;', recorder)
        self.assertLess(recorder.index('config.gpio_cfg.dout = I2S_GPIO_UNUSED;'),
                recorder.index('i2s_channel_init_std_mode(micHandle, &config)'))
        self.assertNotIn('i2s_channel_reconfig_std_gpio', playback)
        self.assertIn('startDacOutput(outputHandle, dacConfig)', playback)
        self.assertIn('stopDacOutput(outputHandle, enabled)', playback)
        self.assertIn('Serial.printf("Radio DAC GPIO12/13/15: %s\\n"', main)
        self.assertIn('Serial.printf("I2S microphone GPIO6/7/8: %s\\n"', main)
        self.assertIn('Serial.printf("Playback error: %s\\n"', main)

    def test_serial_radio_playback_is_opt_in_and_reports_first_i2s_block(self):
        root = WEB_ROOT.parent
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        playback = (root / "src" / "playback.cpp").read_text(encoding="utf-8")
        script = (root / "scripts" / "test_device.py").read_text(encoding="utf-8")
        self.assertIn("if (command == 'P')", main)
        self.assertIn('safeSdPath(path)', main)
        self.assertIn('playbackStart(path, PlaybackTarget::radio)', main)
        self.assertIn("if (command == 'Z')", main)
        self.assertIn('Radio DAC first block:', playback)
        self.assertIn('parser.add_argument("--play-radio"', script)
        self.assertIn('device.write(b"P" + path.encode("utf-8") + b"\\n")', script)
        self.assertIn('device.write(b"Z")', script)
        self.assertIn("if (command == 'L')", main)
        self.assertIn('Serial.printf("WAV: %s', main)
        self.assertIn('parser.add_argument("--list-wav"', script)

    def test_file_manager_explains_unavailable_radio_and_keeps_play_request_errors(self):
        script = (WEB_ROOT / "files.js").read_text(encoding="utf-8")
        self.assertIn('Radio DAC: unavailable', script)
        self.assertIn('playbackRequestError = error.message', script)
        self.assertIn('data.playbackError || playbackRequestError', script)
        self.assertIn('button.title = !data.radioReady', script)
        self.assertIn('Playback finished', script)

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
        self.assertIn('case 12: drawText(9, y, "Timezone")', display)
        self.assertIn('if (!appClockSetLocal(value))', display)
        self.assertIn('server.on("/api/time", HTTP_POST, setClock)', server)
        self.assertIn('server.on("/api/time/zone", HTTP_POST, setClockZone)', server)

    def test_rtc_boot_sync_and_diagnostics_contract(self):
        root = WEB_ROOT.parent
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        display = (root / "src" / "display.cpp").read_text(encoding="utf-8")
        main = (root / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('Wire.begin(4, 5)', control)
        self.assertIn('rtcReadUtc(rtcEpoch)', control)
        self.assertIn('(status & 0x80)', control)
        self.assertIn('instant = kMinEpoch;', control)
        self.assertIn('rtcValid && (!savedValid || rtcEpoch > static_cast<time_t>(savedEpoch))', control)
        self.assertIn('rtcEpoch > static_cast<time_t>(savedEpoch)', control)
        self.assertIn('rtcWriteUtc(instant)', control)
        self.assertIn('rtcWriteUtc(now)', control)
        self.assertLess(control.index('rtcWriteUtc(now)'), control.index('clockSynced = true;', control.index('void appClockPoll()')))
        self.assertIn('clockFromRtc ? "rtc"', control)
        self.assertIn('appRtcStatus()', main)
        self.assertIn('appRtcReadUtc(rtcEpoch)', main)
        self.assertIn('if (command == \'y\')', main)
        self.assertIn('wifiManagerHasCredentials()', main)
        self.assertIn('RTC_RESTART accepted', main)
        self.assertIn('appRtcStatus()', display)
        self.assertNotIn('Wire.begin(kSdaGpio, kSclGpio)', display)
        for page in ("index.js", "files.js", "settings.js"):
            self.assertIn('rtc:', (WEB_ROOT / page).read_text(encoding="utf-8"))

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
        self.assertIn("if (pttTestActive()) stopPttTest();", display)
        self.assertIn("else if (!latchPttTest())", display)
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

    def test_raw_adc_probe_validates_gpio_and_audio_busy_without_sd_writes(self):
        initial_files = dict(self.server.device.files)
        self.assertEqual(self.request('/api/adc/raw?gpio=4')[0], 400)
        self.assertEqual(self.request('/api/adc/raw?gpio=abc')[0], 400)
        for gpio in (1, 2, 3, 9):
            code, payload, _ = self.request('/api/adc/raw?gpio=' + str(gpio))
            self.assertEqual(code, 200)
            result = json.loads(payload)
            self.assertEqual(result['gpio'], gpio)
            self.assertEqual(len(result['samples']), 128)
            self.assertTrue(all(0 <= sample <= 4095 for sample in result['samples']))
        self.assertEqual(self.server.device.files, initial_files)
        self.server.device.recording = True
        self.assertEqual(self.request('/api/adc/raw?gpio=1')[0], 409)
        self.server.device.recording = False
        self.server.device.signal_enabled = True
        self.assertEqual(self.request('/api/adc/raw?gpio=1')[0], 409)
        self.server.device.signal_enabled = False
        self.server.device.playing = '/REC00001.wav'
        self.server.device.play_until = time.monotonic() + 2
        self.assertEqual(self.request('/api/adc/raw?gpio=1')[0], 409)

    def test_dac_tone_is_timed_stoppable_and_rejects_bad_or_busy_requests(self):
        settings = {"frequency": "1000", "rate": "44100", "bits": "16", "format": "philips", "level": "50"}
        self.assertEqual(self.request("/api/dac-tone", "POST", {**settings, "rate": "96000"})[0], 400)
        self.assertEqual(self.request("/api/dac-tone", "POST", settings)[0], 202)
        self.assertEqual(self.request("/api/dac-tone", "POST", settings)[0], 409)
        status, data, _ = self.request("/api/status")
        playback = json.loads(data)
        self.assertEqual(status, 200)
        self.assertEqual(playback["playbackTarget"], "radio")
        self.assertEqual(playback["playbackBytesTotal"], 44100 * 10 * 4)
        self.assertFalse(self.server.device.ptt_latched)
        self.server.device.play_until -= 5
        self.assertGreater(json.loads(self.request("/api/status")[1])["playbackBytesPlayed"], 0)
        self.assertEqual(self.request("/api/play/stop", "POST")[0], 200)
        self.assertEqual(json.loads(self.request("/api/status")[1])["playback"], "idle")
        self.server.device.recording = True
        self.assertEqual(self.request("/api/dac-tone", "POST", settings)[0], 409)

    def test_removed_diagnostics_are_not_available_in_preview(self):
        self.assertEqual(self.request("/api/dac-demo", "POST")[0], 404)
        self.assertEqual(self.request("/api/wifi-test", "POST")[0], 404)
        self.assertFalse(self.server.device.recording)

    def test_wifi_noise_test_is_absent_but_regular_wifi_remains(self):
        root = WEB_ROOT.parent
        control = (root / "src" / "app_control.cpp").read_text(encoding="utf-8")
        server = (root / "src" / "web_server.cpp").read_text(encoding="utf-8")
        page = render_page("index")
        self.assertNotIn("appPollWifiTest", control)
        self.assertNotIn('server.on("/api/wifi-test"', server)
        self.assertNotIn('id="wifiTest"', page)
        self.assertIn('server.on("/api/wifi/connect"', server)

    def test_radio_volume_and_playback_progress(self):
        status, _, _ = self.request("/api/play/radio-volume?value=23", "POST")
        self.assertEqual(status, 200)
        status, _, _ = self.request("/api/play/radio-volume?value=101", "POST")
        self.assertEqual(status, 400)
        status, _, _ = self.request("/api/play?path=%2FREC00001.wav&target=radio", "POST")
        self.assertEqual(status, 200)
        self.server.device.play_until -= 0.5
        status, body, _ = self.request("/api/status")
        playback = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(playback["radioVolume"], 23)
        self.assertGreater(playback["playbackBytesPlayed"], 0)
        self.assertLess(playback["playbackBytesPlayed"], playback["playbackBytesTotal"])

    def test_radio_boost_accepts_only_supported_values_and_is_independent(self):
        self.assertEqual(json.loads(self.request("/api/status")[1])["radioBoost"], 1)
        self.assertEqual(self.request("/api/play/radio-boost?value=16", "POST")[0], 200)
        for value in ("0", "3", "32", "-1", "wat"):
            self.assertEqual(self.request(f"/api/play/radio-boost?value={value}", "POST")[0], 400)
        status = json.loads(self.request("/api/status")[1])
        self.assertEqual(status["radioBoost"], 16)
        self.assertEqual(status["speakerBoost"], 1)

    def test_radio_dac_playback_reports_target_and_stops_without_ptt(self):
        status, _, _ = self.request("/api/play?path=%2FREC00001.wav&target=invalid", "POST")
        self.assertEqual(status, 400)
        status, _, _ = self.request("/api/play?path=%2FREC00001.wav&target=radio", "POST")
        self.assertEqual(status, 200)
        status, data, _ = self.request("/api/status")
        self.assertEqual(status, 200)
        playback = json.loads(data)
        self.assertTrue(playback["radioReady"])
        self.assertEqual(playback["playbackTarget"], "radio")
        self.assertEqual(playback["playback"], "playing")
        self.assertFalse(self.server.device.ptt_latched)
        status, _, _ = self.request("/api/play/stop", "POST")
        self.assertEqual(status, 200)
        status, data, _ = self.request("/api/status")
        self.assertEqual(json.loads(data)["playback"], "idle")

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
        for path, name in (("/", "index"), ("/files", "files"), ("/settings", "settings"), ("/test", "test"), ("/radio", "radio"), ("/dac-test", "dac_test")):
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

    def test_radio_read_requires_confirmation_and_interlocks_ptt(self):
        self.assertEqual(json.loads(self.request('/api/radio')[1])["state"], "idle")
        self.assertEqual(self.request('/api/radio/image')[0], 409)
        self.assertEqual(self.request('/api/radio/read', 'GET')[0], 404)
        self.assertEqual(self.request('/api/radio/read', 'POST')[0], 400)
        self.assertEqual(self.request('/api/ptt-test/latch', 'POST')[0], 200)
        self.assertEqual(self.request('/api/radio/read', 'POST', {"confirm": "1"})[0], 409)
        self.request('/api/ptt-test/off', 'POST')
        self.assertEqual(self.request('/api/radio/read', 'POST', {"confirm": "1"})[0], 202)
        self.assertEqual(self.request('/api/radio/read', 'POST', {"confirm": "1"})[0], 409)
        self.assertEqual(self.request('/api/ptt-test/on', 'POST')[0], 409)
        self.assertEqual(self.request('/api/ptt-test/latch', 'POST')[0], 409)
        self.assertEqual(self.request('/api/radio/image')[0], 409)
        self.server.device.radio_started -= 3
        state = json.loads(self.request('/api/radio')[1])
        self.assertEqual(state["state"], "done")
        self.assertEqual(state["channels"][0]["rxHz"], 145500000)
        status, payload, headers = self.request('/api/radio/image')
        self.assertEqual(status, 200)
        self.assertIn(b'PREVIEW ONLY', payload)
        self.assertIn('preview-only.bin', headers['Content-Disposition'])
        self.assertEqual(self.request('/api/radio/write', 'POST')[0], 400)

    def test_radio_write_is_revision_bound_and_requires_explicit_confirmation(self):
        device = self.server.device
        self.assertEqual(device.api("POST", "/api/radio/write", {})[0], 400)
        device.radio_started = time.monotonic() - 3
        revision = device.api("GET", "/api/radio", {})[1]["revision"]
        patch = {"revision": revision, "changes": [{"group": "settings", "field": "squelch", "value": 7}]}
        self.assertEqual(device.api("POST", "/api/radio/write", {"confirm": "WRITE UV5R", "patch": json.dumps(patch)})[0], 202)
        self.assertEqual(device.api("POST", "/api/ptt-test/on", {})[0], 409)
        device.radio_started -= 3
        state = device.api("GET", "/api/radio", {})[1]
        self.assertTrue(state["writeVerified"])
        self.assertEqual(state["settings"]["squelch"], 7)
        self.assertNotEqual(state["revision"], revision)
        self.assertEqual(device.api("POST", "/api/radio/write", {"confirm": "WRITE UV5R", "patch": json.dumps(patch)})[0], 409)

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

    def test_language_setting_validates_and_restores_saved_selection(self):
        for path in ('/', '/files', '/settings', '/test', '/radio', '/dac-test'):
            status, content, _ = self.request(path)
            self.assertEqual(status, 200)
            self.assertIn(b'window.RR_LANGUAGES', content)
            self.assertIn(b'window.RR_I18N', content)
        self.assertEqual(json.loads(self.request('/api/settings')[1])["language"], "hu")
        self.assertEqual(self.request('/api/language', 'POST', {"language": "en"})[0], 200)
        self.assertEqual(json.loads(self.request('/api/settings')[1])["language"], "en")
        for language in ("", "invalid", "EN", "../en"):
            with self.subTest(language=language):
                self.assertEqual(self.request('/api/language', 'POST', {"language": language})[0], 400)
                self.assertEqual(json.loads(self.request('/api/settings')[1])["language"], "en")
        self.server.device = PreviewDevice(saved_language="en")
        self.assertEqual(json.loads(self.request('/api/settings')[1])["language"], "en")
        self.assertEqual(self.request('/api/language', 'POST', {"language": "hu"})[0], 200)
        self.assertEqual(json.loads(self.request('/api/settings')[1])["language"], "hu")

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