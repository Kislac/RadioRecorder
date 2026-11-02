"""Deterministic single-file automatic capture model for the offline preview."""

import copy
import math
import time

MODES = ("manualMic", "manualRadio", "autoRadio", "autoMic")
DEFAULTS = {"radio": {"threshold": 100, "silenceSeconds": 3},
            "microphone": {"threshold": 100, "silenceSeconds": 3}, "priorities": list(MODES)}


def valid_settings(settings):
    if not isinstance(settings, dict) or not isinstance(settings.get("priorities"), list) or len(settings["priorities"]) != 4 or any(type(mode) is not str for mode in settings["priorities"]) or set(settings["priorities"]) != set(MODES):
        return False
    for source, maximum in (("radio", 4095), ("microphone", 32768)):
        values = settings.get(source)
        if not isinstance(values, dict):
            return False
        threshold, silence = values.get("threshold"), values.get("silenceSeconds")
        if type(threshold) is not int or type(silence) is not int or not 1 <= threshold <= maximum or not 1 <= silence <= 60:
            return False
    return True


class PreviewAutomaticRecording:
    def __init__(self, device, saved=None):
        self.device = device
        selected = saved if valid_settings(saved) else DEFAULTS
        self.settings = {"priorities": list(selected["priorities"]), **{source: {key: selected[source][key] for key in ("threshold", "silenceSeconds")} for source in ("radio", "microphone")}}
        self.mode = "manualRadio" if device.recording else "none"
        self.started = device.started_at * 1000
        self.last_tick = self.started
        self.sources = {source: {"armed": False, "rms": 0, "armedAt": self.started, "lastSound": self.started,
                                 "above": 0, "lastFile": "", "lastElapsedMs": 0, "error": "", "blocked": ""}
                        for source in ("radio", "microphone")}
        self.rms_override = {}
        self.last_elapsed = 0

    def elapsed(self, now=None):
        if not self.device.recording:
            return self.last_elapsed
        now = time.monotonic() * 1000 if now is None else now
        return max(0, int(now - self.started)) + (500 if self.mode in ("autoRadio", "autoMic") else 0)

    def begin(self, mode, now):
        if self.device.recording:
            raise RuntimeError("Only one recording file is allowed")
        source = "microphone" if mode in ("manualMic", "autoMic") else "adc"
        self.mode = mode
        self.device.record_source = source
        self.device.recording_name = self.device.next_recording_name(source, mode in ("autoRadio", "autoMic"))
        self.device.recording = True
        self.started = now
        self.sources["microphone" if source == "microphone" else "radio"]["lastSound"] = now

    def finish(self, now):
        if not self.device.recording:
            self.mode = "none"
            return
        mode = self.mode
        elapsed = self.elapsed(now)
        path = self.device.recording_name
        self.device.finish_recording(elapsed)
        self.last_elapsed = elapsed
        if mode in ("autoRadio", "autoMic"):
            source = self.sources["microphone" if mode == "autoMic" else "radio"]
            source.update(lastFile=path, lastElapsedMs=elapsed, above=0)
        self.mode = "none"

    def manual(self, source):
        mode = "manualMic" if source == "microphone" else "manualRadio"
        if not self.device.sd_ready or self.device.playing or self.device.file_manager.busy():
            return False
        if self.device.recording and (self.mode == mode or self.settings["priorities"].index(mode) > self.settings["priorities"].index(self.mode)):
            return False
        now = time.monotonic() * 1000
        self.finish(now)
        self.begin(mode, now)
        return True

    def stop(self):
        if not self.device.recording:
            return False
        if self.mode in ("autoRadio", "autoMic"):
            self.sources["microphone" if self.mode == "autoMic" else "radio"]["armed"] = False
        self.finish(time.monotonic() * 1000)
        return True

    def tick(self, now):
        end = now * 1000
        if end - self.last_tick > 600000:
            self.last_tick = end - 600000
        while self.last_tick + 100 <= end:
            self.last_tick += 100
            self.step(self.last_tick)

    def step(self, now):
        blocked = "SD unavailable" if not self.device.sd_ready else "File operation busy" if self.device.file_manager.busy() else "Playback active" if self.device.playing else ""
        requested = {mode: False for mode in MODES}
        if self.mode in ("manualMic", "manualRadio") and self.device.recording:
            requested[self.mode] = True
        for name, mode in (("radio", "autoRadio"), ("microphone", "autoMic")):
            source, config = self.sources[name], self.settings[name]
            source["blocked"] = blocked
            if not source["armed"] or blocked:
                if blocked:
                    source.update(above=0, armedAt=now)
                continue
            rms = self.rms_override.get(name, int((1200 + 500 * math.sin(now / 1000 * 2)) if name == "microphone" else (35 + 20 * math.sin(now / 1000 * 2))))
            source["rms"] = rms
            if now - source["armedAt"] < 2000:
                source.update(above=0, lastSound=now)
                continue
            active = self.device.recording and self.mode == mode
            threshold = math.ceil(config["threshold"] * 0.8) if active else config["threshold"]
            if rms >= threshold:
                source.update(above=min(2, source["above"] + 1), lastSound=now)
            else:
                source["above"] = 0
            requested[mode] = now - source["lastSound"] < config["silenceSeconds"] * 1000 if active else source["above"] >= 2
        desired = next((mode for mode in self.settings["priorities"] if requested[mode]), "none")
        if desired != self.mode:
            self.finish(now)
            if desired != "none":
                self.begin(desired, now)

    def status(self):
        result = {"activeMode": self.mode, "priorities": list(self.settings["priorities"])}
        for name, mode in (("radio", "autoRadio"), ("microphone", "autoMic")):
            source = self.sources[name]
            recording = self.device.recording and self.mode == mode
            result[name] = {key: source[key] for key in ("armed", "rms", "lastFile", "lastElapsedMs", "error", "blocked")}
            result[name].update(self.settings[name], recording=recording, elapsedMs=self.elapsed() if recording else 0)
            if source["armed"] and self.device.recording and not recording:
                result[name]["blocked"] = "Higher priority recording active"
        return result

    def api(self, method, path, params):
        if method == "GET" and path == "/api/auto-record":
            return 200, self.status()
        if method == "POST" and path == "/api/auto-record/settings":
            if not valid_settings(params):
                return 400, "Invalid automatic settings or priorities"
            self.settings = {"priorities": list(params["priorities"]), **{source: {key: params[source][key] for key in ("threshold", "silenceSeconds")} for source in self.sources}}
            for source in self.sources.values():
                source["above"] = 0
            return 200, self.status()
        if method == "POST" and path == "/api/auto-record/arm":
            name, enabled = params.get("source"), params.get("enabled")
            if name not in self.sources or enabled not in ("0", "1"):
                return 400, "Invalid automatic source or state"
            if enabled == "1" and not self.device.sd_ready:
                return 409, "Cannot arm automatic recording"
            now = time.monotonic() * 1000
            self.sources[name].update(armed=enabled == "1", armedAt=now, lastSound=now, above=0, error="")
            self.step(now)
            return 200, self.status()
        return None