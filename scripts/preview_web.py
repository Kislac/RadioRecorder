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

from scripts.web_assets import WEB_ROOT

SCENARIOS = ("ready", "missing-sd", "recording", "wifi-failure")
DEFAULT_EPOCH = 946684800


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
    def __init__(self, scenario="ready", saved_clock=None):
        if scenario not in SCENARIOS:
            raise ValueError("Unknown preview scenario")
        self.lock = threading.RLock()
        self.scenario = scenario
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
        self.boost = 1
        self.screen_minutes = 5
        self.wifi_minutes = 5
        self.wav = sample_wav()
        self.files = {"/REC00001.wav": self.wav, "/REC00002.wav": self.wav,
                  "/Archive/sample.wav": self.wav}
        self.directories = {"/Archive"}
        self.recording_name = self.next_recording_name(self.record_source) if self.recording else ""
        self.playing = ""
        self.play_until = 0
        self.auto_stop_at = 0
        self.ptt_until = 0
        self.ptt_latched = False
        self.radio_started = None
        self.scan_started = None
        self.pending_network = None
        self.wifi = {"mode": "AP", "ssid": "RadioRecorder", "ip": "192.168.4.1", "apIp": "192.168.4.1",
                     "savedSsid": "", "hasSavedNetwork": False, "keepApHost": False,
                     "connecting": False, "apAvailable": True, "autoOffMinutes": 5}

    def tick(self):
        now = time.monotonic()
        if self.clock_epoch is not None:
            instant = self.clock_epoch + (now - self.clock_started_at)
            day = clock_local(instant, self.clock_zone).date()
            if day != self.saved_day:
                self.saved_epoch, self.saved_day = int(instant), day
        if self.mic_signal_enabled and now - self.mic_signal_polled_at > 4:
            self.mic_signal_enabled = False
        if self.playing and now >= self.play_until:
            self.playing = ""
        if self.auto_stop_at and now >= self.auto_stop_at:
            self.finish_recording()
        if self.ptt_until and now >= self.ptt_until:
            self.ptt_until = 0
        if self.pending_network and now >= self.pending_network[0]:
            _, ssid, keep_ap = self.pending_network
            self.pending_network = None
            self.wifi["connecting"] = False
            if self.scenario == "wifi-failure":
                self.wifi.update(mode="AP", ssid="RadioRecorder", ip="192.168.4.1", apAvailable=True)
            else:
                self.wifi.update(mode="AP+STA" if keep_ap else "STA", ssid=ssid, ip="192.168.1.80",
                                 savedSsid=ssid, hasSavedNetwork=True, keepApHost=keep_ap, apAvailable=keep_ap)
                self.clock_epoch = self.saved_epoch = int(time.time())
                self.clock_started_at = now
                self.clock_source = "internet"
                self.saved_day = clock_local(self.clock_epoch, self.clock_zone).date()

    def clock_info(self):
        epoch = int(self.clock_epoch + time.monotonic() - self.clock_started_at) if self.clock_epoch is not None else 0
        local = clock_local(epoch, self.clock_zone).strftime("%Y-%m-%dT%H:%M:%S") if epoch else ""
        return {"local": local, "epoch": epoch, "zone": self.clock_zone, "source": self.clock_source}

    def finish_recording(self):
        self.recording = False
        self.auto_stop_at = 0
        self.files[self.recording_name] = self.wav

    def next_recording_name(self, source):
        local = clock_local(self.clock_info()["epoch"], self.clock_zone)
        folder = "/" + local.strftime("%Y-%m-%d")
        prefix = "MicMan" if source == "microphone" else "RadioMan"
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
        elapsed = time.monotonic() - self.started_at
        if method == "GET":
            if path == "/api/radio":
                reading = self.radio_started is not None and time.monotonic() - self.radio_started < 2
                state = {"state": "idle" if self.radio_started is None else "reading" if reading else "done",
                         "error": "", "bytesRead": 0 if self.radio_started is None else 2048 if reading else 6464,
                         "totalBytes": 6464, "rxGpio": 16, "txGpio": 17}
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
                return 200, state
            if path == "/api/radio/image":
                if self.radio_started is None or time.monotonic() - self.radio_started < 2:
                    return 409, "No complete radio image"
                return 200, b"PREVIEW ONLY - NOT A RADIO IMAGE"
            if path == "/api/ptt-test":
                return 200, {"active": bool(self.ptt_until or self.ptt_latched), "pulseMs": 1000}
            if path == "/api/status":
                clock = self.clock_info()
                return 200, {"version": "preview", "ready": self.sd_ready, "micReady": self.sd_ready,
                             "clock": clock, "demoSeconds": sum(int(value) * scale for value, scale in
                             zip(clock["local"][11:19].split(":"), (3600, 60, 1))) if clock["local"] else 0,
                             "recordSource": self.record_source, "meterReady": True,
                             "liveReady": True, "audioGpio": 1, "screenOffMinutes": self.screen_minutes,
                             "lowpass": self.lowpass,
                             "state": "recording" if self.recording else "idle", "stopping": False,
                             "speakerReady": True, "speakerVolume": self.volume, "speakerBoost": self.boost,
                             "playback": "playing" if self.playing else "idle", "playbackPath": self.playing,
                             "playbackError": "", "hasRecordings": bool(self.files) and self.sd_ready,
                             "error": "" if self.sd_ready else "SD unavailable", "wifi": dict(self.wifi)}
            if path == "/api/settings":
                return 200, {"screenOffMinutes": self.screen_minutes, "wifiAutoOffMinutes": self.wifi_minutes,
                             "wifi": dict(self.wifi)}
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
                         if re.fullmatch(r"(?:/REC\d{5}|/\d{4}-\d{2}-\d{2}/(?:Radio|Mic)(?:Man|Aut)\d{6}_\d{6}(?:_\d{2})?)\.wav", name)]
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
            if path == "/api/time/zone":
                self.clock_zone = self.integer(params, "zone", 0, 28)
                if self.clock_epoch is not None:
                    self.saved_day = clock_local(self.clock_info()["epoch"], self.clock_zone).date()
                return 200, self.clock_info()
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
            if path in ("/api/start", "/api/mic/start", "/api/wifi-test"):
                if not self.sd_ready or self.recording or self.playing:
                    return 409, "Recorder unavailable or busy"
                self.recording = True
                self.record_source = "microphone" if path == "/api/mic/start" else "adc"
                self.recording_name = self.next_recording_name(self.record_source)
                if path == "/api/wifi-test":
                    self.auto_stop_at = time.monotonic() + 2
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
                self.finish_recording()
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
                self.screen_minutes, self.wifi_minutes = screen, wifi
                self.wifi["autoOffMinutes"] = wifi
                return 200, {"screenOffMinutes": screen, "wifiAutoOffMinutes": wifi}
            if path == "/api/display/wake":
                return 200, "Display awake (simulated)"
            if path == "/api/play/volume":
                self.volume = self.integer(params, "value", 0, 100)
                return 200, str(self.volume)
            if path == "/api/play/boost":
                boost = self.integer(params, "value", 1, 4)
                if boost not in (1, 2, 4):
                    raise ValueError("Boost must be 1, 2 or 4")
                self.boost = boost
                return 200, str(boost)
            if path == "/api/play":
                if self.recording or not self.sd_ready or self.playing:
                    return 409, "Speaker unavailable or busy"
                if params.get("path") not in self.files:
                    return 404, "WAV not found"
                self.playing, self.play_until = params["path"], time.monotonic() + 2
                return 200, "Playing on simulated speaker"
            if path == "/api/play/stop":
                self.playing = ""
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
                if self.clock_source == "internet":
                    self.clock_source = "saved"
                self.wifi.update(mode="AP", ssid="RadioRecorder", ip="192.168.4.1", savedSsid="",
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

    def dispatch(self, method):
        url = urlsplit(self.path)
        params = {key: values[-1] for key, values in parse_qs(url.query, keep_blank_values=True).items()}
        if method == "POST":
            length = int(self.headers.get("Content-Length", "0"))
            if length > 8192:
                self.reply(413, "Request too large")
                return
            params.update({key: values[-1] for key, values in
                           parse_qs(self.rfile.read(length).decode("utf-8"), keep_blank_values=True).items()})
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
                    if url.path == "/api/radio/image" and status == 200:
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
                          "/test": "test.html", "/radio": "radio.html"}
                name = routes.get(url.path, url.path.lstrip("/"))
                allowed = {page + extension for page in ("index", "files", "settings", "radio")
                           for extension in (".html", ".css", ".js")}
                allowed.update(("test.html", "test.js"))
                if name in allowed:
                    source = (WEB_ROOT / name).read_text(encoding="utf-8")
                    if name.endswith(".html"):
                        source = source.replace("<head>", "<head><script>window.radioRecorderAudioUrl="
                                                "'ws://'+location.host+'/preview/live';</script>", 1)
                        source = source.replace("<title>", "<title>[Offline] ", 1)
                    mime = "text/html" if name.endswith(".html") else "text/css" if name.endswith(".css") else "text/javascript"
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