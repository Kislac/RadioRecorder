"""Local-only RadioRecorder web preview with an in-memory device simulator."""

import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
from pathlib import Path
import re
import select
import struct
import threading
import time
from urllib.parse import parse_qs, urlsplit
import wave
import zipfile

from scripts.web_assets import WEB_ROOT, load_languages, localize_document
from scripts.preview_files import PreviewFileManager
from scripts.preview_auto import PreviewAutomaticRecording

SCENARIOS = ("ready", "missing-sd", "recording", "wifi-failure", "wifi-no-ntp")
DEFAULT_EPOCH = 946684800


def radio_schema():
    groups = {
        "settings": {"squelch": 9, "save": 4, "vox": 10, "backlight": 24, "dualWatch": 1,
                     "beep": 1, "timeoutRaw": 39, "voiceRaw": 2, "dtmfSidetoneRaw": 3,
                     "scanResumeRaw": 2, "pttIdRaw": 3, "pttDelayRaw": 50, "displayARaw": 2,
                     "displayBRaw": 2, "busyLockRaw": 1, "autoLockRaw": 1, "standbyLedRaw": 3,
                     "rxLedRaw": 3, "txLedRaw": 3, "alarmModeRaw": 2, "txPriorityRaw": 2,
                     "tailEliminateRaw": 1, "repeaterTailRaw": 10, "repeaterDelayRaw": 10,
                     "powerOnModeRaw": 1, "rogerRaw": 1, "activeLineRaw": 1,
                     "fmRadioEnabledRaw": 1, "workModeRaw": 1, "keypadLockRaw": 1,
                     "channelA": 127, "channelB": 127},
        "vfo": {"frequencyHz": 520000000, "offsetHz": 999999000, "rxToneRaw": 65535,
                "txToneRaw": 65535, "shiftRaw": 2, "signalCodeRaw": 14, "stepRaw": 7,
                "powerRaw": 1, "bandwidthRaw": 1},
        "channel": {"rxHz": 520000000, "txHz": 520000000, "rxToneRaw": 65535, "txToneRaw": 65535,
                    "signalCodeRaw": 14, "powerRaw": 1, "bandwidthRaw": 1, "busyLockRaw": 1,
                    "scanRaw": 1, "pttIdRaw": 3, "name": 7},
        "dtmf": {"aniCode": 5, "alarmCode": 3, "aniModeRaw": 3, "toneOnRaw": 195, "toneOffRaw": 195,
                 **{f"code{index}": 5 for index in range(1, 16)}}}
    return {"fields": [{"group": group, "field": field, "max": maximum,
                         "kind": "text" if field in ("name", "aniCode", "alarmCode") or field.startswith("code")
                         else "tone" if field.endswith("ToneRaw") else "frequency" if field.endswith("Hz") else "number"}
                        for group, values in groups.items() for field, maximum in values.items()]}


def safe_sd_path(path):
    return (path == "/" or path.startswith("/") and len(path) <= 220 and
            all(segment not in ("", ".", "..") and not any(ord(character) < 32 or character in '\\:\"\x7f'
                                                          for character in segment)
                for segment in path[1:].split("/")))


def last_sunday(year, month):
    last = datetime(year, month + 1, 1) - timedelta(days=1)
    return last - timedelta(days=(last.weekday() + 1) % 7)


def clock_local(epoch, zone):
    instant = datetime.fromtimestamp(epoch, timezone.utc)
    offset = zone - 14 if zone >= 2 else 0
    if zone == 0:
        year = instant.year
        starts = last_sunday(year, 3).replace(hour=1, tzinfo=timezone.utc)
        ends = last_sunday(year, 10).replace(hour=1, tzinfo=timezone.utc)
        offset = 2 if starts <= instant < ends else 1
    return (instant + timedelta(hours=offset)).replace(tzinfo=None)


def sample_wav():
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(22050)
        audio.writeframes(b"".join(struct.pack("<h", int(2200 * math.sin(2 * math.pi * 440 * frame / 22050)))
                                   for frame in range(44100)))
    return output.getvalue()


class PreviewDevice:
    def __init__(self, scenario="ready", saved_clock=None, saved_language="hu", saved_auto=None):
        if scenario not in SCENARIOS:
            raise ValueError("Unknown preview scenario")
        self.lock = threading.RLock()
        self.scenario = scenario
        self.language = saved_language if saved_language in load_languages() else "hu"
        self.started_at = time.monotonic()
        self.saved_epoch, self.clock_zone = saved_clock or (None, 0)
        self.clock_epoch = self.saved_epoch if self.saved_epoch is not None else DEFAULT_EPOCH
        self.clock_started_at = self.started_at
        self.clock_source = "saved" if self.saved_epoch is not None else "default"
        self.saved_day = clock_local(self.clock_epoch, self.clock_zone).date()
        self.recording = scenario == "recording"
        self.record_source = "adc"
        self.sd_ready = scenario != "missing-sd"
        self.signal_enabled = False
        self.mic_signal_enabled = False
        self.mic_signal_polled_at = 0
        self.mic_signal_started_at = 0
        self.lowpass = False
        self.volume = 25
        self.radio_volume = 5
        self.radio_boost = 1
        self.radio_tot = 60
        self.replay_rules = []
        self.replay_next_id = 1
        self.replay_pending = False
        self.boost = 1
        self.screen_minutes = 5
        self.performance_enabled = True
        self.performance_log_enabled = self.sd_ready and not self.recording
        self.performance_log_path = ""
        self.record_cues = {mode: {"start": 1 if mode.startswith("auto") else 2,
                       "stop": 1 if mode.startswith("auto") else 2}
                    for mode in ("manualMic", "manualRadio", "autoRadio", "autoMic")}
        self.wifi_minutes = 5
        self.wav = sample_wav()
        self.files = {"/REC00001.wav": self.wav, "/REC00002.wav": self.wav,
                  "/Archive/sample.wav": self.wav}
        self.directories = {"/Archive"}
        self.file_manager = PreviewFileManager(self)
        self.recording_name = self.next_recording_name(self.record_source) if self.recording else ""
        self.playing = ""
        self.playback_target = "speaker"
        self.play_until = 0
        self.play_paused_at = 0
        self.tone_bytes_total = 0
        self.ptt_until = 0
        self.ptt_latched = False
        self.radio_started = None
        self.radio_revision = 1
        self.radio_changes = []
        self.radio_write = False
        self.scan_started = None
        self.pending_network = None
        self.wifi = {"mode": "AP", "ssid": "RadioRecorder", "ip": "192.168.4.1", "apSsid": "RadioRecorder", "apIp": "192.168.4.1", "staSsid": "", "staIp": "",
                     "savedSsid": "", "hasSavedNetwork": False, "keepApHost": False,
                     "connecting": False, "apAvailable": True, "autoOffMinutes": 5}
        self.automatic = PreviewAutomaticRecording(self, saved_auto)
        self.performance_logs = {}
        if self.sd_ready:
            self.performance_logs["/perf-20261103-120000-v0.1.92-00.txt"] = True
            if self.performance_log_enabled:
                self.performance_log_path = "/perf-20261104-080000-v0.1.93-00.txt"
                self.performance_logs[self.performance_log_path] = True
        self.saved_performance = self.api("GET", "/api/performance", {})[1]

    def tick(self):
        now = time.monotonic()
        if self.clock_epoch is not None:
            instant = self.clock_epoch + (now - self.clock_started_at)
            day = clock_local(instant, self.clock_zone).date()
            if day != self.saved_day:
                self.saved_epoch, self.saved_day = int(instant), day
        if self.mic_signal_enabled and now - self.mic_signal_polled_at > 4:
            self.mic_signal_enabled = False
        if self.playing and not self.play_paused_at and self.play_until and now >= self.play_until:
            self.playing = ""
        if self.ptt_until and now >= self.ptt_until:
            self.ptt_until = 0
        if self.pending_network and now >= self.pending_network[0]:
            _, ssid, keep_ap = self.pending_network
            self.pending_network = None
            self.wifi["connecting"] = False
            if self.scenario == "wifi-failure":
                self.wifi.update(mode="AP", ssid="RadioRecorder", ip="192.168.4.1", apSsid="RadioRecorder", apIp="192.168.4.1", staSsid="", staIp="", apAvailable=True)
            else:
                self.wifi.update(mode="AP+STA" if keep_ap else "STA", ssid=ssid, ip="192.168.1.80",
                                 apSsid="RadioRecorder" if keep_ap else "", apIp="192.168.4.1" if keep_ap else "",
                                 staSsid=ssid, staIp="192.168.1.80", savedSsid=ssid, hasSavedNetwork=True,
                                 keepApHost=keep_ap, apAvailable=keep_ap)
                if self.scenario != "wifi-no-ntp":
                    self.clock_epoch = self.saved_epoch = int(time.time())
                    self.clock_started_at = now
                    self.clock_source = "internet"
                    self.saved_day = clock_local(self.clock_epoch, self.clock_zone).date()

    def clock_info(self):
        epoch = int(self.clock_epoch + time.monotonic() - self.clock_started_at) if self.clock_epoch is not None else 0
        local = clock_local(epoch, self.clock_zone).strftime("%Y-%m-%dT%H:%M:%S") if epoch else ""
        return {"local": local, "epoch": epoch, "zone": self.clock_zone, "source": self.clock_source,
            "uptimeSeconds": max(0, int(time.monotonic() - self.started_at))}

    def finish_recording(self, elapsed_ms=None):
        self.recording = False
        if elapsed_ms is None:
            self.files[self.recording_name] = self.wav
            return
        samples = int(max(0, elapsed_ms) * 22050 / 1000)
        pcm = self.wav[44:]
        content = (pcm * ((samples * 2 + len(pcm) - 1) // len(pcm)))[:samples * 2]
        output = io.BytesIO()
        with wave.open(output, "wb") as recording:
            recording.setnchannels(1); recording.setsampwidth(2); recording.setframerate(22050); recording.writeframes(content)
        self.files[self.recording_name] = output.getvalue()

    def next_recording_name(self, source, automatic=False):
        local = clock_local(self.clock_info()["epoch"], self.clock_zone)
        folder = "/" + local.strftime("%Y-%m-%d")
        prefix = ("MicAut" if automatic else "MicMan") if source == "microphone" else ("RadioAut" if automatic else "RadioMan")
        stem = folder + "/" + prefix + local.strftime("%y%m%d_%H%M%S")
        for number in range(100):
            path = stem + (f"_{number:02d}" if number else "") + ".wav"
            if path not in self.files:
                self.directories.add(folder)
                return path
        raise ValueError("No free recording filename")

    def folder_zip(self, path):
        if not safe_sd_path(path) or path not in self.directories:
            raise ValueError("Invalid folder")
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as archive:
            for name, data in self.files.items():
                if name.startswith(path + "/"):
                    archive.writestr(path.rsplit("/", 1)[-1] + name[len(path):], data)
        return output.getvalue()

    @staticmethod
    def integer(params, name, minimum, maximum):
        text = params.get(name, "")
        if not text.isascii() or not text.isdigit() or not minimum <= int(text) <= maximum:
            raise ValueError(f"{name} must be {minimum}-{maximum}")
        return int(text)

    def api(self, method, path, params):
        self.tick()
        if path.startswith("/api/automatic-replay"):
            if method == "GET" and path == "/api/automatic-replay":
                return 200, {"armed": False, "armPending": self.replay_pending, "storageValid": self.sd_ready,
                             "queueLength": 0, "error": "", "rules": [dict(rule) for rule in self.replay_rules]}
            if method == "POST" and path == "/api/automatic-replay/stop":
                self.replay_pending = False
                return self.api("GET", "/api/automatic-replay", {})
            if method == "POST" and path == "/api/automatic-replay/arm":
                if params.get("confirm") != "MEASURED TOT":
                    return 403, "Measured TOT required"
                if not any(rule["active"] for rule in self.replay_rules):
                    return 409, "No active rule"
                self.replay_pending = True
                return 202, self.api("GET", "/api/automatic-replay", {})[1]
            if method == "POST" and path == "/api/automatic-replay/rules":
                if not self.sd_ready or self.replay_pending or len(self.replay_rules) >= 16:
                    return 409, "SD or replay busy"
                params = dict(params)
                if "delaySeconds" in params:
                    delay = params.pop("delaySeconds")
                    if "anchorUtc" in params or params.get("intervalSeconds") != 0 or type(delay) is not int or not 1 <= delay <= 31536000:
                        return 400, "Invalid relative delay or clock"
                    params["anchorUtc"] = int(time.time()) + delay
                required = ("path", "anchorUtc", "intervalSeconds", "jitterMinutes", "window", "startMinute", "endMinute")
                if not all(key in params for key in required):
                    return 400, "Invalid WAV path or schedule"
                folder = params.get("folder", False)
                if type(folder) is not bool or (folder and (params["path"] not in self.directories and params["path"] != "/")) or (not folder and (params["path"] not in self.files or not params["path"].lower().endswith(".wav"))):
                    return 400, "Invalid WAV path or schedule"
                if any(type(params[key]) is not int for key in required if key != "path") or not 1577836800 <= params["anchorUtc"] <= 4102444800 or not 0 <= params["intervalSeconds"] <= 604800 or params["jitterMinutes"] < 0 or params["jitterMinutes"] * 120 >= params["intervalSeconds"] > 0:
                    return 400, "Invalid WAV path or schedule"
                rule = {key: params[key] for key in required}
                if folder:
                    if any(type(params.get(key)) is not int or params[key] not in (0, 1) for key in ("order", "cycle", "batch")):
                        return 400, "Invalid folder options"
                    rule.update(folder=True, order=params["order"], cycle=params["cycle"], batch=params["batch"],
                                cursor="", fileCount=0, fingerprint=0)
                rule.update(id=self.replay_next_id, active=False, completed=False, nextUtc=params["anchorUtc"])
                self.replay_next_id += 1
                self.replay_rules.append(rule)
                return self.api("GET", "/api/automatic-replay", {})
            if method == "POST" and path == "/api/automatic-replay/activate":
                rule = next((item for item in self.replay_rules if str(item["id"]) == str(params.get("id"))), None)
                if not rule or str(params.get("enabled")) not in ("0", "1"):
                    return 400, "Invalid rule id or state"
                if str(params["enabled"]) == "1" and rule.get("folder"):
                    prefix = rule["path"].rstrip("/") + "/"
                    children = [name for name in self.files if name.startswith(prefix) and
                                "/" not in name[len(prefix):] and name.lower().endswith(".wav")]
                    if not children or len(children) > 128:
                        return 409, "Folder unavailable"
                    rule["fileCount"] = len(children)
                elif str(params["enabled"]) == "1" and rule["path"] not in self.files:
                    return 409, "WAV unavailable"
                rule["active"] = str(params["enabled"]) == "1"
                return self.api("GET", "/api/automatic-replay", {})
            return 404, "Not found"
        self.automatic.tick(time.monotonic())
        automatic = self.automatic.api(method, path, params)
        if automatic is not None:
            return automatic
        managed = self.file_manager.api(method, path, params)
        if managed is not None:
            return managed
        if method == "POST" and path in ("/api/start", "/api/mic/start", "/api/play", "/api/files/delete") and self.file_manager.busy():
            return 409, "File operation busy"
        elapsed = time.monotonic() - self.started_at
        if method == "GET":
            if path == "/api/radio/schema":
                return 200, radio_schema()
            if path == "/api/radio":
                reading = self.radio_started is not None and time.monotonic() - self.radio_started < 2
                state = {"state": "idle" if self.radio_started is None else "reading" if reading else "done",
                         "error": "", "bytesRead": 0 if self.radio_started is None else 2048 if reading else 6464,
                         "totalBytes": 6464, "rxGpio": 16, "txGpio": 17}
                state.update(revision=f"{self.radio_revision:08x}" if state["state"] == "done" else "",
                             writeSupported=state["state"] == "done", writeVerified=self.radio_write and not reading,
                             operation="write" if self.radio_write else "read", stage="preflight" if self.radio_write and reading else "read",
                             bytesWritten=0 if reading else 16 if self.radio_write else 0, writeBytes=16 if self.radio_write else 0)
                if self.radio_write and reading:
                    state["state"] = "writing"
                if state["state"] == "done":
                    state.update(firmware="PREVIEW ONLY", droppedByteWorkaround=False,
                                 settings={"squelch": 3, "save": 1, "vox": 0, "backlight": 5,
                                           "dualWatch": 1, "beep": 0, "timeoutRaw": 3,
                                           "channelA": 0, "channelB": 1, "vfoAHz": 145500000,
                                           "vfoBHz": 433500000},
                                 channels=[{"number": 0, "name": "VHF SIM", "rxHz": 145500000,
                                            "txHz": 145500000, "rxToneRaw": 0, "txToneRaw": 885,
                                            "bandwidth": "Narrow", "powerRaw": 1, "scan": True},
                                           {"number": 1, "name": "UHF SIM", "rxHz": 433500000,
                                            "txHz": None, "rxToneRaw": 1, "txToneRaw": 106,
                                            "bandwidth": "Wide", "powerRaw": 0, "scan": False}])
                    state["settings"].update(stepRaw=1, voiceRaw=1, dtmfSidetoneRaw=3, scanResumeRaw=0,
                                             pttIdRaw=0, pttDelayRaw=5, displayARaw=2, displayBRaw=2,
                                             busyLockRaw=0, autoLockRaw=0, shiftRaw=0, standbyLedRaw=2,
                                             rxLedRaw=0, txLedRaw=0, alarmModeRaw=1, bandRaw=0,
                                             txPriorityRaw=0, tailEliminateRaw=1, repeaterTailRaw=5,
                                             repeaterDelayRaw=0, powerOnModeRaw=1, rogerRaw=0,
                                             rogerRxRaw=1, activeLineRaw=0, fmRadioEnabledRaw=1,
                                             alarmEnabledRaw=1, resetMenuEnabledRaw=1, menusEnabledRaw=1,
                                             workModeRaw=0, keypadLockRaw=0)
                    state.update(vfoA={"frequencyHz": 145500000, "offsetHz": 600000, "rxToneRaw": 0,
                                       "txToneRaw": 885, "bandRaw": 0, "shiftRaw": 2, "signalCodeRaw": 0,
                                       "stepRaw": 1, "powerRaw": 1, "bandwidthRaw": 1},
                                 vfoB={"frequencyHz": 433500000, "offsetHz": 0, "rxToneRaw": 1,
                                       "txToneRaw": 106, "bandRaw": 1, "shiftRaw": 0, "signalCodeRaw": 1,
                                       "stepRaw": 4, "powerRaw": 0, "bandwidthRaw": 0},
                                 dtmf={"aniCode": "12345", "alarmCode": "123", "aniModeRaw": 0,
                                       "toneOnRaw": 3, "toneOffRaw": 3},
                                 startup={"line1": "PREVIEW", "line2": "ONLY", "alternateLine1": "SIMULAT",
                                          "alternateLine2": "ED DATA"})
                    for index in range(1, 16):
                        state["dtmf"][f"code{index}"] = f"{index:05d}"
                    for channel in state["channels"]:
                        channel.update(busyLockRaw=0, pttIdRaw=0, signalCodeRaw=0)
                    for change in self.radio_changes:
                        target = (next(channel for channel in state["channels"] if channel["number"] == change["index"])
                                  if change["group"] == "channel" else state[change["group"]])
                        field, value = change["field"], change["value"]
                        if change["group"] == "channel" and field == "bandwidthRaw":
                            target["bandwidth"] = "Wide" if value else "Narrow"
                        elif change["group"] == "channel" and field == "scanRaw":
                            target["scan"] = bool(value)
                        else:
                            target[field] = value
                return 200, state
            if path == "/api/radio/image":
                if self.radio_started is None or time.monotonic() - self.radio_started < 2:
                    return 409, "No complete radio image"
                return 200, b"PREVIEW ONLY - NOT A RADIO IMAGE"
            if path == "/api/ptt-test":
                return 200, {"active": bool(self.ptt_until or self.ptt_latched), "pulseMs": 1000}
            if path == "/api/adc/raw":
                if params.get("gpio") not in ("1", "2", "3", "9"):
                    return 400, "Invalid ADC GPIO"
                if self.recording or self.signal_enabled or self.playing:
                    return 409, "ADC busy: stop recording, live audio or signal monitoring"
                gpio = int(params["gpio"])
                center = {1: 2048, 2: 960, 3: 2750, 9: 1580}[gpio]
                samples = [round(center + 90 * math.sin(index * math.tau / 32 + elapsed * 3))
                           for index in range(128)]
                return 200, {"gpio": gpio, "samples": samples}
            if path == "/api/status":
                clock = self.clock_info()
                tone = self.playing.startswith("DAC tone")
                pcm_total = self.tone_bytes_total if tone else len(self.files[self.playing]) - 44 if self.playing else 0
                seconds = self.tone_duration if tone else 2
                playback_now = self.play_paused_at or time.monotonic()
                if tone and not seconds:
                    pcm_played = int((playback_now - self.tone_started_at) * self.tone_rate * self.tone_channels_bytes)
                else:
                    pcm_played = min(pcm_total, max(0, int((playback_now - (self.play_until - seconds)) * pcm_total / seconds))) if self.playing else 0
                return 200, {"version": "preview", "ready": self.sd_ready, "micReady": self.sd_ready,
                             "automatic": self.automatic.status(), "recordingMode": self.automatic.mode,
                             "recordElapsedMs": self.automatic.elapsed(), "fileBusy": self.file_manager.busy(),
                             "clock": clock, "demoSeconds": sum(int(value) * scale for value, scale in
                             zip(clock["local"][11:19].split(":"), (3600, 60, 1))) if clock["local"] else 0,
                             "recordSource": self.record_source, "meterReady": True,
                             "liveReady": True, "audioGpio": 1, "screenOffMinutes": self.screen_minutes,
                             "lowpass": self.lowpass,
                             "state": "recording" if self.recording else "idle", "stopping": False,
                             "speakerReady": True, "radioReady": True,
                             "speakerVolume": self.volume, "speakerBoost": self.boost,
                             "radioVolume": self.radio_volume, "radioBoost": self.radio_boost,
                             "playbackBytesPlayed": pcm_played, "playbackBytesTotal": pcm_total,
                             "playbackPaused": bool(self.play_paused_at),
                             "radioPttActive": bool(self.playing and not self.play_paused_at and self.playback_target == "radio"),
                             "playback": "playing" if self.playing else "idle", "playbackPath": self.playing,
                             "playbackTarget": self.playback_target,
                             "playbackError": "", "hasRecordings": bool(self.files) and self.sd_ready,
                             "error": "" if self.sd_ready else "SD unavailable", "wifi": dict(self.wifi)}
            if path == "/api/settings":
                return 200, {"screenOffMinutes": self.screen_minutes, "wifiAutoOffMinutes": self.wifi_minutes,
                             "radioTotSeconds": self.radio_tot,
                             "wifi": dict(self.wifi), "language": self.language,
                             "performanceEnabled": self.performance_enabled}
            if path == "/api/performance":
                if not self.performance_enabled:
                    return 200, {"enabled": False}
                return 200, {"enabled": True, "preview": True,
                             "uptimeMs": int((time.monotonic() - self.started_at) * 1000),
                             "loopMaxGapUs": 10000, "loopGapsOver100ms": 0, "taskCount": 5,
                             "loopCurrentUs": 2100, "loopAverageUs": 2300, "loopPeakUs": 12000,
                             "loopSamples": 1200, "displayCurrentUs": 8200,
                             "displayAverageUs": 8500, "displayPeakUs": 16000, "displaySamples": 80,
                             "logEnabled": self.performance_log_enabled, "logPath": self.performance_log_path,
                             "logError": "", "logBytes": 900 if self.performance_log_path else 0,
                             "logSamples": 1 if self.performance_log_path else 0, "logSkippedBusy": 0,
                             "logBuildUs": 1200, "logWriteUs": 4000,
                             "loopStackFreeMinimum": 4096, "recorderStackFreeMinimum": 4096,
                             "speakerStackFreeMinimum": 4096, "lastFileWorkerStackFreeMinimum": 0,
                             "adcOverflows": 0, "liveAudioDroppedFrames": 0,
                             "internalFree": 120000, "internalTotal": 320000, "internalMinimumFree": 100000,
                             "internalLargestBlock": 80000, "psramFree": 4000000, "psramTotal": 8000000,
                             "psramMinimumFree": 3800000, "psramLargestBlock": 3000000,
                             "nvsTotalEntries": 500, "nvsUsedEntries": 30, "nvsFreeEntries": 440}
            if path == "/api/performance/logs":
                if not self.sd_ready or self.recording or self.playing:
                    return 409, "SD or audio busy"
                return 200, {"files": [{"path": name, "bytes": 900} for name in self.performance_logs]}
            if path == "/api/performance/log":
                if not self.sd_ready or self.recording or self.playing:
                    return 409, "SD or audio busy"
                selected = params.get("path", self.performance_log_path)
                if not isinstance(selected, str) or not re.fullmatch(r"/perf-[A-Za-z0-9.-]+\.txt", selected):
                    return 400, "Invalid log path"
                if selected not in self.performance_logs:
                    return 404, "Log not found"
                return 200, json.dumps({"timestampUtc": "2026-11-04T08:01:00Z", "firmware": "preview", "metrics": self.saved_performance}) + "\n"
            if path == "/api/record-cues":
                return 200, {mode: dict(events) for mode, events in self.record_cues.items()}
            if path == "/api/time":
                return 200, self.clock_info()
            if path == "/api/wifi/status":
                return 200, dict(self.wifi)
            if path == "/api/wifi/scan":
                scanning = self.scan_started is not None and time.monotonic() - self.scan_started < 0.6
                state = "idle" if self.scan_started is None else "scanning" if scanning else "complete"
                return 200, {"state": state, "networks": [] if state != "complete" else [
                    {"ssid": "Workshop WiFi", "rssi": -42, "open": False},
                    {"ssid": "Guest network", "rssi": -65, "open": True}]}
            if path == "/api/signal":
                rms = int(35 + 20 * math.sin(elapsed * 2)) if self.signal_enabled else 0
                return 200, {"enabled": self.signal_enabled, "active": self.signal_enabled,
                             "sequence": int(elapsed * 10), "rms": rms, "peak": rms * 3, "bias": 2048, "fault": 0}
            if path == "/api/spectrum":
                source = params.get("source")
                if source not in ("radio", "microphone"):
                    return 400, "Invalid source"
                if not ((self.recording and self.record_source == ("microphone" if source == "microphone" else "adc")) or
                        (self.mic_signal_enabled if source == "microphone" else self.signal_enabled)):
                    return 204, b""
                return 200, b"".join(struct.pack("<h", round(6000 * math.sin(index * math.tau * 1000 / 22050)))
                                     for index in range(512))
            if path == "/api/play/waveform":
                name = params.get("path", "")
                if not safe_sd_path(name) or not name.lower().endswith(".wav"):
                    return 400, "Invalid WAV path"
                if self.recording or self.playing or self.file_manager.busy():
                    return 409, "SD busy"
                if name not in self.files:
                    return 404, "WAV not found"
                try:
                    with wave.open(io.BytesIO(self.files[name])) as audio:
                        if audio.getnchannels() != 1 or audio.getsampwidth() != 2 or audio.getframerate() != 22050:
                            return 415, "Unsupported PCM WAV"
                        samples = audio.readframes(audio.getnframes())
                except wave.Error:
                    return 415, "Unsupported PCM WAV"
                peaks = [max((abs(struct.unpack_from("<h", samples, offset)[0]) for offset in
                            range(index * (len(samples) // 128) & ~1,
                                min(len(samples), (index * (len(samples) // 128) & ~1) + 2048), 2)), default=0)
                         for index in range(128)]
                return 200, {"bytes": len(samples), "peaks": peaks}
            if path == "/api/mic/signal":
                if self.mic_signal_enabled:
                    self.mic_signal_polled_at = time.monotonic()
                active = self.mic_signal_enabled and (not self.recording or self.record_source == "microphone")
                rms = int(1200 + 500 * math.sin(elapsed * 2)) if active else 0
                return 200, {"enabled": self.mic_signal_enabled, "active": active,
                             "sequence": int((time.monotonic() - self.mic_signal_started_at) * 10) if active else 0,
                             "rms": rms, "peak": rms * 3, "fault": 0}
            if path in ("/api/recordings", "/api/files"):
                if self.recording or not self.sd_ready:
                    return 409, "SD unavailable or recording in progress"
                offset = self.integer({"offset": params.get("offset", "0")}, "offset", 0, 100000)
                files = [{"name": name.rsplit("/", 1)[-1], "path": name, "bytes": len(data)}
                         for name, data in reversed(list(self.files.items()))
                         if name.lower().endswith(".wav") and len(data) >= 44]
                if path == "/api/recordings":
                    return 200, {"files": files[offset:offset + 20],
                                 "next": offset + 20 if offset + 20 < len(files) else None}
                directory = params.get("dir", "/")
                if not safe_sd_path(directory) or directory != "/" and directory not in self.directories:
                    return 404, "Directory not found"
                items = ([{"name": name.rsplit("/", 1)[-1], "path": name, "directory": True, "bytes": 0}
                          for name in sorted(self.directories) if name.rsplit("/", 1)[0] == directory.rstrip("/")] +
                         [{"name": name.rsplit("/", 1)[-1], "path": name, "directory": False, "bytes": len(data)}
                          for name, data in self.files.items() if name.rsplit("/", 1)[0] == directory.rstrip("/")])
                return 200, {"items": items[offset:offset + 20],
                             "next": offset + 20 if offset + 20 < len(items) else None}
        if method == "POST":
            if path == "/api/time/auto":
                text = params.get("epoch", "")
                if not re.fullmatch(r"\d{10}", text) or not 1577836800 <= int(text) <= 4102444799:
                    return 400, "Invalid UTC epoch"
                epoch = int(text)
                current = self.clock_info()["epoch"]
                if self.clock_source in ("default", "saved", "phone") and abs(epoch - current) >= 30:
                    self.clock_epoch = self.saved_epoch = epoch
                    self.clock_started_at = time.monotonic()
                    self.clock_source = "phone"
                    self.saved_day = clock_local(epoch, self.clock_zone).date()
                return 200, self.clock_info()
            if path == "/api/time/zone":
                self.clock_zone = self.integer(params, "zone", 0, 28)
                if self.clock_epoch is not None:
                    self.saved_day = clock_local(self.clock_info()["epoch"], self.clock_zone).date()
                return 200, self.clock_info()
            if path == "/api/language":
                language = params.get("language", "")
                if language not in load_languages():
                    return 400, "Unsupported language"
                self.language = language
                return 200, {"language": self.language}
            if path == "/api/time":
                text = params.get("local", "")
                try:
                    local = datetime.strptime(text, "%Y-%m-%dT%H:%M")
                    if local.strftime("%Y-%m-%dT%H:%M") != text or not 2020 <= local.year <= 2099:
                        raise ValueError()
                    offsets = (1, 2) if self.clock_zone == 0 else (self.clock_zone - 14 if self.clock_zone >= 2 else 0,)
                    candidates = [int((local - timedelta(hours=offset)).replace(tzinfo=timezone.utc).timestamp())
                                  for offset in offsets]
                    epoch = next(value for value in candidates if clock_local(value, self.clock_zone) == local)
                except (ValueError, StopIteration):
                    return 400, "Invalid date/time"
                self.clock_epoch = self.saved_epoch = epoch
                self.clock_started_at = time.monotonic()
                self.clock_source = "manual"
                self.saved_day = local.date()
                return 200, self.clock_info()
            if path == "/api/radio/read":
                if params.get("confirm") != "1":
                    return 400, "Confirm verified wiring with confirm=1"
                if (self.ptt_until or self.ptt_latched or
                        self.radio_started is not None and time.monotonic() - self.radio_started < 2):
                    return 409, "Radio read or PTT already active"
                self.radio_started = time.monotonic()
                self.radio_write = False
                return 202, self.api("GET", "/api/radio", {})[1]
            if path == "/api/radio/write":
                if params.get("confirm") != "WRITE UV5R":
                    return 400, "WRITE UV5R confirmation required"
                state = self.api("GET", "/api/radio", {})[1]
                if state["state"] != "done" or self.ptt_until or self.ptt_latched:
                    return 409, "Read radio first; radio or PTT busy"
                patch = json.loads(params.get("patch", "{}"))
                if (not isinstance(patch, dict) or set(patch) != {"revision", "changes"} or
                        patch["revision"] != state["revision"] or not isinstance(patch["changes"], list) or
                        not 1 <= len(patch["changes"]) <= 32):
                    return 409, "Invalid patch or stale revision"
                schema = radio_schema()["fields"]
                for change in patch["changes"]:
                    if not isinstance(change, dict) or not {"group", "field", "value"} <= set(change):
                        return 400, "Invalid change"
                    group = "vfo" if change["group"] in ("vfoA", "vfoB") else change["group"]
                    field = next((field for field in schema if field["group"] == group and field["field"] == change["field"]), None)
                    if not field or not set(change) <= {"group", "field", "value", "index"}:
                        return 400, "Unsupported field"
                    if group == "channel" and (type(change.get("index")) is not int or change["index"] not in (0, 1)):
                        return 400, "Channel unavailable"
                    value = change["value"]
                    if field["kind"] == "text":
                        if not isinstance(value, str) or len(value) > field["max"] or not value.isascii():
                            return 400, "Invalid text"
                    elif value is None and group == "channel" and change["field"] == "txHz":
                        pass
                    elif type(value) is not int or not 0 <= value <= field["max"]:
                        return 400, "Invalid number"
                self.radio_changes.extend(patch["changes"])
                self.radio_revision += 1
                self.radio_started = time.monotonic()
                self.radio_write = True
                return 202, self.api("GET", "/api/radio", {})[1]
            if path == "/api/ptt-test/on":
                if self.radio_started is not None and time.monotonic() - self.radio_started < 2:
                    return 409, "Radio read active"
                if self.ptt_until or self.ptt_latched:
                    return 409, "PTT test already active"
                self.ptt_until = time.monotonic() + 1
                return 200, "PTT test pulse started"
            if path == "/api/ptt-test/latch":
                if self.radio_started is not None and time.monotonic() - self.radio_started < 2:
                    return 409, "Radio read active"
                if self.ptt_until or self.ptt_latched:
                    return 409, "PTT already active"
                self.ptt_latched = True
                return 200, "PTT relay on"
            if path == "/api/ptt-test/off":
                self.ptt_until = 0
                self.ptt_latched = False
                return 200, "PTT test off"
            if path in ("/api/start", "/api/mic/start"):
                if not self.automatic.manual("microphone" if path == "/api/mic/start" else "adc"):
                    return 409, "Recorder unavailable or busy"
                return 200, self.recording_name
            if path == "/api/files/delete":
                target = params.get("path", "")
                if not safe_sd_path(target) or target == "/" or params.get("confirm") != "1":
                    return 400, "Invalid path or missing confirmation"
                if not self.sd_ready or self.recording or self.playing:
                    return 409, "SD, recorder or playback busy"
                if target in self.directories:
                    self.directories = {name for name in self.directories if not name.startswith(target + "/") and name != target}
                    self.files = {name: data for name, data in self.files.items() if not name.startswith(target + "/")}
                elif target in self.files:
                    del self.files[target]
                else:
                    return 404, "File or folder not found"
                return 200, {"deleted": True}
            if path == "/api/stop":
                if not self.recording:
                    return 409, "Not recording"
                self.automatic.stop()
                return 200, "Stopping"
            if path in ("/api/filter/on", "/api/filter/off"):
                self.lowpass = path.endswith("/on")
                return 200, "OK"
            if path in ("/api/signal/on", "/api/signal/off"):
                if path.endswith("/on") and self.mic_signal_enabled:
                    return 409, "Microphone monitoring active"
                self.signal_enabled = path.endswith("/on")
                return 200, "OK"
            if path in ("/api/mic/signal/on", "/api/mic/signal/off"):
                if not self.sd_ready:
                    return 503, "Microphone unavailable"
                if path.endswith("/on"):
                    if self.signal_enabled or (self.recording and self.record_source != "microphone"):
                        return 409, "Audio monitoring busy"
                    self.mic_signal_started_at = time.monotonic()
                    self.mic_signal_polled_at = self.mic_signal_started_at
                self.mic_signal_enabled = path.endswith("/on")
                return 200, "OK"
            if path == "/api/settings":
                screen = self.integer(params, "screenOffMinutes", 0, 999)
                wifi = self.integer({"wifiAutoOffMinutes": params.get("wifiAutoOffMinutes", str(self.wifi_minutes))},
                                    "wifiAutoOffMinutes", 0, 999)
                tot = self.integer({"radioTotSeconds": params.get("radioTotSeconds", str(self.radio_tot))},
                                   "radioTotSeconds", 10, 600)
                if tot != self.radio_tot and self.playing and self.playback_target == "radio":
                    return 500, "Radio TOT cannot change during transmission"
                self.screen_minutes, self.wifi_minutes = screen, wifi
                self.radio_tot = tot
                self.wifi["autoOffMinutes"] = wifi
                return 200, {"screenOffMinutes": screen, "wifiAutoOffMinutes": wifi, "radioTotSeconds": tot}
            if path == "/api/performance":
                if params.get("enabled") not in ("0", "1"):
                    return 400, "Invalid enabled value"
                self.performance_enabled = params["enabled"] == "1"
                if not self.performance_enabled:
                    self.performance_log_enabled = False
                return self.api("GET", path, {})
            if path == "/api/performance/log":
                if params.get("enabled") not in ("0", "1"):
                    return 400, "Invalid enabled value"
                if params["enabled"] == "1" and not self.performance_enabled:
                    return 409, "Diagnostics off"
                self.performance_log_enabled = params["enabled"] == "1"
                if self.performance_log_enabled and not self.performance_log_path:
                    self.performance_log_path = "/perf-preview-{:02d}.txt".format(len(self.performance_logs))
                    self.performance_logs[self.performance_log_path] = True
                return self.api("GET", "/api/performance", {})
            if path == "/api/record-cues":
                if set(params) != set(self.record_cues) or any(
                    not isinstance(params[mode], dict) or set(params[mode]) != {"start", "stop"} or
                    any(type(value) is not int or value not in (0, 1, 2) for value in params[mode].values())
                    for mode in self.record_cues
                ):
                    return 400, "Invalid recording cues"
                self.record_cues = {mode: dict(events) for mode, events in params.items()}
                return 200, {mode: dict(events) for mode, events in self.record_cues.items()}
            if path == "/api/display/wake":
                return 200, "Display awake (simulated)"
            if path == "/api/play/volume":
                self.volume = self.integer(params, "value", 0, 100)
                return 200, str(self.volume)
            if path == "/api/play/radio-volume":
                self.radio_volume = self.integer(params, "value", 0, 100)
                return 200, str(self.radio_volume)
            if path == "/api/play/radio-boost":
                gain = self.integer(params, "value", 1, 16)
                if gain not in (1, 2, 4, 8, 16):
                    raise ValueError("Radio gain must be 1, 2, 4, 8 or 16")
                self.radio_boost = gain
                return 200, str(gain)
            if path == "/api/play/boost":
                boost = self.integer(params, "value", 1, 4)
                if boost not in (1, 2, 4):
                    raise ValueError("Boost must be 1, 2 or 4")
                self.boost = boost
                return 200, str(boost)
            if path == "/api/play":
                target = params.get("target", "speaker")
                if target not in ("speaker", "radio"):
                    return 400, "Invalid playback target"
                if self.recording or not self.sd_ready or self.playing:
                    return 409, "Audio output unavailable or busy"
                if params.get("path") not in self.files:
                    return 404, "WAV not found"
                self.playback_target = target
                self.playing, self.play_until = params["path"], time.monotonic() + 2
                self.play_paused_at = 0
                return 200, "Playing on simulated " + target
            if path == "/api/play/pause":
                if params.get("paused") not in ("0", "1"):
                    return 400, "Invalid pause setting"
                if not self.playing or self.playing.startswith("DAC tone"):
                    return 409, "Playback unavailable"
                if params["paused"] == "1" and not self.play_paused_at:
                    self.play_paused_at = time.monotonic()
                elif params["paused"] == "0" and self.play_paused_at:
                    self.play_until += time.monotonic() - self.play_paused_at
                    self.play_paused_at = 0
                return 200, "Playback pause"
            if path == "/api/play/seek":
                offset = self.integer(params, "offset", 0, 0xffffffff)
                if not self.playing or self.playing.startswith("DAC tone") or offset >= len(self.files[self.playing]) - 44:
                    return 409, "Playback seek unavailable"
                self.play_until = (self.play_paused_at or time.monotonic()) + (len(self.files[self.playing]) - 44 - offset) / 44100
                return 200, "Playback seek"
            if path == "/api/dac-tone":
                frequency, rate, bits, format_name, level, duration = (params.get(key, "") for key in
                                                                      ("frequency", "rate", "bits", "format", "level", "duration"))
                if (frequency not in ("400", "1000", "2000") or rate not in ("22050", "44100", "48000") or
                        bits not in ("16", "32") or format_name not in ("philips", "msb") or
                        level not in ("20", "50", "90") or duration not in ("10", "30", "60", "0")):
                    return 400, "Invalid DAC tone settings"
                if self.recording or self.playing or self.mic_signal_enabled:
                    return 409, "Stop recording, playback or live audio first"
                self.playback_target = "radio"
                self.playing = f"DAC tone {frequency} Hz / {rate} Hz / {bits} bit / {format_name} / {level}%"
                self.tone_started_at = time.monotonic()
                self.tone_duration = int(duration)
                self.tone_rate = int(rate)
                self.tone_channels_bytes = 2 * (int(bits) // 8)
                self.play_until = self.tone_started_at + self.tone_duration if self.tone_duration else 0
                self.tone_bytes_total = self.tone_rate * self.tone_duration * self.tone_channels_bytes
                return 202, "DAC tone started"
            if path == "/api/play/stop":
                self.playing = ""
                self.play_paused_at = 0
                return 200, "Stopping speaker"
            if path == "/api/wifi/scan":
                self.scan_started = time.monotonic()
                return 202, {"state": "scanning"}
            if path == "/api/wifi/connect":
                ssid = params.get("ssid", "")
                if not 1 <= len(ssid.encode("utf-8")) <= 32 or len(params.get("password", "")) > 64:
                    raise ValueError("Invalid network credentials")
                keep_ap = params.get("keepApHost") in ("1", "true", "on")
                self.wifi.update(mode="CONNECTING", connecting=True)
                self.pending_network = (time.monotonic() + 0.8, ssid, keep_ap)
                return 202, {"state": "connecting"}
            if path == "/api/wifi/forget":
                self.pending_network = None
                self.wifi.update(mode="AP", ssid="RadioRecorder", ip="192.168.4.1", apSsid="RadioRecorder", apIp="192.168.4.1", staSsid="", staIp="", savedSsid="",
                                 hasSavedNetwork=False, keepApHost=False, connecting=False, apAvailable=True)
                return 200, {"state": "ap"}
        return 404, "Not found"


class PreviewServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, scenario="ready"):
        self.device = PreviewDevice(scenario)
        super().__init__(address, PreviewHandler)


class PreviewHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, status, value, mime="application/json", headers=None):
        if isinstance(value, (dict, list)):
            value = json.dumps(value, ensure_ascii=False).encode("utf-8")
        elif isinstance(value, str):
            value = value.encode("utf-8")
            if mime == "application/json":
                mime = "text/plain; charset=utf-8"
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(value)))
        self.send_header("Cache-Control", "no-store")
        for key, content in (headers or {}).items():
            self.send_header(key, content)
        self.end_headers()
        self.wfile.write(value)

    def do_GET(self):
        self.dispatch("GET")

    def do_POST(self):
        self.dispatch("POST")

    def do_PUT(self):
        self.dispatch("PUT")

    def dispatch(self, method):
        url = urlsplit(self.path)
        params = {key: values[-1] for key, values in parse_qs(url.query, keep_blank_values=True).items()}
        if method == "PUT" and url.path == "/transfer/upload":
            self.server.device.file_manager.upload(self, int(params.get("id", 0)))
            return
        if method == "GET" and url.path == "/transfer/zip":
            self.server.device.file_manager.stream_zip(self, int(params.get("id", 0)))
            return
        if method == "POST":
            length = int(self.headers.get("Content-Length", "0"))
            if length > 8192:
                self.reply(413, "Request too large")
                return
            body = self.rfile.read(length).decode("utf-8")
            if url.path == "/api/radio/write":
                params["patch"] = body
            elif self.headers.get("Content-Type", "").startswith("application/json"):
                try:
                    values = json.loads(body) if body else {}
                    if not isinstance(values, dict):
                        raise ValueError("JSON object required")
                    params.update(values)
                except ValueError:
                    self.reply(400, "Invalid JSON request")
                    return
            else:
                params.update({key: values[-1] for key, values in parse_qs(body, keep_blank_values=True).items()})
        if url.path == "/preview/live" and method == "GET":
            self.stream_audio()
            return
        try:
            with self.server.device.lock:
                if url.path.startswith("/api/"):
                    if url.path == "/api/files/download" and method == "GET":
                        folder = params.get("path", "")
                        if not self.server.device.sd_ready or self.server.device.recording:
                            self.reply(409, "SD or recorder busy")
                        else:
                            archive = self.server.device.folder_zip(folder)
                            self.reply(200, archive, "application/zip", {"Content-Disposition":
                                       'attachment; filename="' + folder.rsplit("/", 1)[-1] + '.zip"'})
                        return
                    status, payload = self.server.device.api(method, url.path, params)
                    if url.path == "/api/spectrum" and status == 200:
                        self.reply(status, payload, "application/octet-stream")
                    elif url.path == "/api/radio/image" and status == 200:
                        self.reply(status, payload, "application/octet-stream",
                                   {"Content-Disposition": 'attachment; filename="preview-only.bin"'})
                    else:
                        self.reply(status, payload)
                    return
                if url.path == "/file" and method == "GET":
                    self.send_audio(params)
                    return
            if method == "GET":
                routes = {"/": "index.html", "/files": "files.html", "/settings": "settings.html",
                          "/test": "test.html", "/radio": "radio.html", "/dac-test": "dac_test.html",
                          "/automatic-replay": "automatic_replay.html"}
                name = routes.get(url.path, url.path.lstrip("/"))
                allowed = {page + extension for page in ("index", "files", "settings", "radio", "dac_test")
                           for extension in (".html", ".css", ".js")}
                allowed.update(("test.html", "test.js", "live_audio.js", "radio_edit.js", "file_actions.js", "automatic.js"))
                allowed.update(("station.css", "logo.svg"))
                if name in allowed:
                    source = (WEB_ROOT / name).read_text(encoding="utf-8")
                    if name.endswith(".html"):
                        source = localize_document(source)
                        source = source.replace("<head>", "<head><script>window.radioRecorderAudioUrl="
                                                "'ws://'+location.host+'/preview/live';</script>", 1)
                        source = source.replace("<title>", "<title>[Offline] ", 1)
                    mime = "text/html" if name.endswith(".html") else "text/css" if name.endswith(".css") else "image/svg+xml" if name.endswith(".svg") else "text/javascript"
                    self.reply(200, source, mime + "; charset=utf-8")
                    return
            self.reply(404, "Not found")
        except (ValueError, UnicodeError) as error:
            self.reply(400, str(error))

    def send_audio(self, params):
        device = self.server.device
        if not device.sd_ready or device.recording:
            self.reply(409, "SD unavailable or recording in progress")
            return
        name = params.get("path", "/" + params.get("name", ""))
        data = device.files.get(name) if safe_sd_path(name) else None
        if data is None:
            self.reply(404, "File not found")
            return
        headers = {"Accept-Ranges": "bytes"}
        if params.get("download") == "1":
            headers["Content-Disposition"] = 'attachment; filename="' + Path(name).name + '"'
        status = 200
        byte_range = self.headers.get("Range")
        if byte_range:
            try:
                if not byte_range.startswith("bytes=") or "," in byte_range:
                    raise ValueError()
                first, last = byte_range[6:].split("-", 1)
                start = int(first) if first else max(0, len(data) - int(last))
                end = min(int(last), len(data) - 1) if first and last else len(data) - 1
                if start < 0 or start >= len(data) or end < start:
                    raise ValueError()
                headers["Content-Range"] = f"bytes {start}-{end}/{len(data)}"
                data = data[start:end + 1]
                status = 206
            except ValueError:
                self.reply(416, "Invalid byte range", headers={"Content-Range": f"bytes */{len(data)}"})
                return
        self.reply(status, data, "audio/wav", headers)

    def stream_audio(self):
        if self.headers.get("Upgrade", "").lower() != "websocket" or not self.headers.get("Sec-WebSocket-Key"):
            self.reply(400, "WebSocket upgrade required")
            return
        key = self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        accept = base64.b64encode(hashlib.sha1(key.encode("ascii")).digest()).decode("ascii")
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        payload = self.server.device.wav[44:44 + 2205 * 2]
        packet = b"\x82\x7e" + struct.pack("!H", len(payload)) + payload
        try:
            while True:
                self.wfile.write(packet)
                self.wfile.flush()
                readable, _, _ = select.select([self.connection], [], [], 0.1)
                if readable:
                    incoming = self.connection.recv(4096)
                    if not incoming or incoming[0] & 15 == 8:
                        break
        except (ConnectionError, OSError):
            pass
        self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--scenario", choices=SCENARIOS, default="ready")
    args = parser.parse_args()
    port = args.port
    for attempt in range(20):
        try:
            server = PreviewServer(("127.0.0.1", port), args.scenario)
            break
        except OSError:
            if port == 0 or attempt == 19:
                raise
            port += 1
    print(f"Offline preview: http://127.0.0.1:{server.server_port}  scenario={args.scenario}", flush=True)
    print("Simulated device only; no ESP, serial, Wi-Fi or SD access. Ctrl+C stops the preview.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()