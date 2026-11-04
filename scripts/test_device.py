"""Serial hardware-in-the-loop checks; WAV recording is opt-in via --record."""

import argparse
import json
import re
import time

from scripts.oled_viewer import body_pixels, find_device, read_frame
from scripts.test_support import TestRun, firmware_at_least, validate_performance_snapshot


SIGNAL_PATTERN = re.compile(r"GPIO1 RMS: (\d+) \| Peak: (\d+) \| DC-offset: (\d+) \(ADC\)")
RTC_PATTERN = re.compile(r"RTC_TIME rtc=(\d+) system=(\d+) source=([A-Z-]) offline=([01])")
DISPLAY_TILES = ("Manual radio record", "Manual mic record", "Auto radio record",
                 "Auto mic record", "File manager", "Radio signal", "Microphone signal",
                 "Diagnostics", "PTT control", "Wi-Fi", "Time", "Settings")
SETTINGS_TILE = DISPLAY_TILES.index("Settings")
SETTING_COUNT = 24


def verify_rtc_progress(before, after, elapsed, reboot=False):
    if not before["offline"] or not after["offline"] or "N" in (before["source"], after["source"]):
        raise AssertionError("RTC check requires AP-only mode with no saved station/NTP")
    if reboot and after["source"] != "R":
        raise AssertionError("ESP did not restore its time from RTC after reboot")
    if abs(before["rtc"] - before["system"]) > 3 or abs(after["rtc"] - after["system"]) > 3:
        raise AssertionError("RTC and ESP system time differ")
    advanced = after["rtc"] - before["rtc"]
    if advanced < 1 or advanced > elapsed + 3:
        raise AssertionError("RTC did not advance with elapsed wall time")
    return "RTC advanced {}s in {:.1f}s; ESP time agrees{}".format(
        advanced, elapsed, " after software reboot" if reboot else "")


def rtc_snapshot(device, report):
    device.write(b"q")
    lines = read_until(device, lambda line: line.startswith(("RTC_TIME ", "RTC_TIME_ERROR ")),
                       report=report)
    match = RTC_PATTERN.fullmatch(lines[-1])
    if not match:
        raise AssertionError("RTC did not return valid UTC time: " + lines[-1])
    rtc, system, source, offline = match.groups()
    return {"rtc": int(rtc), "system": int(system), "source": source, "offline": offline == "1"}


def rtc_test_eligible(snapshot):
    return snapshot["offline"] and snapshot["source"] != "N"


def rtc_live_check(device, report, reboot=False):
    before = rtc_snapshot(device, report)
    if not rtc_test_eligible(before):
        raise AssertionError("NTP-free test requires AP-only mode and no saved STA credentials")
    if abs(before["rtc"] - before["system"]) > 3:
        raise AssertionError("ESP clock and RTC disagree before test")
    started = time.monotonic()
    if reboot:
        device.write(b"y")
        response = read_until(device, lambda line: line.startswith("RTC_RESTART "), timeout=3, report=report)
        if response[-1] != "RTC_RESTART accepted":
            raise AssertionError(response[-1])
        read_until(device, lambda line: line == "RadioRecorder setup starting", timeout=10, report=report)
        boot = read_until(device, lambda line: line.startswith("Wi-Fi mode: "), timeout=20, report=report)
        if boot[-1] != "Wi-Fi mode: AP" or any("connecting to saved SSID" in line for line in boot):
            raise AssertionError("Boot was not AP-only; NTP-free RTC test invalid")
    else:
        device.timeout = 0.25
        try:
            while time.monotonic() - started < 2.2:
                line = device.readline().decode("utf-8", errors="replace").strip()
                if line:
                    report.detail("SERIAL: " + line)
        finally:
            device.timeout = 1
    after = rtc_snapshot(device, report)
    return verify_rtc_progress(before, after, time.monotonic() - started, reboot=reboot)


def rtc_firmware_version(device, report):
    device.write(b"i")
    lines = read_until(device, lambda line: line.startswith("Wi-Fi mode: "), report=report)
    firmware = next((line.split("Firmware: ", 1)[1] for line in lines if line.startswith("Firmware: v")), None)
    if not firmware:
        raise AssertionError("Firmware version missing from serial status")
    return firmware


def read_until(device, predicate, timeout=3, report=None):
    lines = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            if report:
                report.detail("SERIAL: " + line)
            if predicate(line):
                return lines
    raise TimeoutError("Expected serial response not received: " + repr(lines))


def status(device, report, display_only=False, radio_only=False):
    device.write(b"i")
    lines = []
    wifi_status_seen = False
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            report.detail("SERIAL: " + line)
            wifi_status_seen |= line.startswith("Wi-Fi IP:")
        elif wifi_status_seen:
            break
    if not wifi_status_seen:
        raise TimeoutError("Incomplete firmware status: " + repr(lines))
    if not any(line.startswith("Firmware: v") for line in lines):
        raise AssertionError("No firmware status from ESP")
    required = (("SD card", "Radio DAC GPIO12/13/15") if radio_only else ("OLED",) if display_only
                else ("SD card", "ADC GPIO1", "Recorder", "OLED", "Speaker"))
    for label in required:
        if not any(line.startswith(label + ": ready") for line in lines):
            raise AssertionError(label + " is not ready: " + repr(lines))
    if not any(line.startswith("Watchdog: active (10 s") for line in lines):
        raise AssertionError("Task watchdog is not active: " + repr(lines))
    timeout_line = next((line for line in lines if line.startswith("OLED screen off: ")), None)
    if timeout_line is None or not 0 <= int(timeout_line.split(": ", 1)[1].split()[0]) <= 999:
        raise AssertionError("Invalid OLED screen timeout status: " + repr(lines))
    wifi_timeout_line = next((line for line in lines if line.startswith("Wi-Fi auto-off: ")), None)
    if wifi_timeout_line is None or not 0 <= int(wifi_timeout_line.split(": ", 1)[1].split()[0]) <= 999:
        raise AssertionError("Invalid Wi-Fi auto-off status: " + repr(lines))
    if any(line.startswith("Recording:") for line in lines):
        raise AssertionError("Stop the current recording before running device tests")
    if radio_only and ("Playback: idle" not in lines or "Playback error: " not in lines and
                       "Playback error:" not in lines):
        raise AssertionError("DAC test requires idle playback without a pending error: " + repr(lines))
    return next(line.split(": ", 1)[1] for line in lines if line.startswith("Firmware: v"))


def test_performance_serial(device, report):
    device.write(b"V")
    lines = read_until(device, lambda line: line.startswith("PERFORMANCE "), report=report)
    snapshot = json.loads(lines[-1].split(" ", 1)[1])
    return validate_performance_snapshot(snapshot)


def performance_preflight(device, report):
    device.write(b"Y")
    lines = read_until(device, lambda line: line.startswith("PERFORMANCE_ENABLE "), report=report)
    if lines[-1] != "PERFORMANCE_ENABLE enabled":
        raise AssertionError("Cannot enable diagnostics: " + lines[-1])
    return performance_postcheck(device, report, require_samples=False)


def performance_postcheck(device, report, require_samples=True):
    device.write(b"V")
    lines = read_until(device, lambda line: line.startswith("PERFORMANCE "), report=report)
    snapshot = json.loads(lines[-1].split(" ", 1)[1])
    validate_performance_snapshot(snapshot)
    if not snapshot["enabled"]:
        raise AssertionError("Diagnostics disabled during online tests")
    if require_samples and (not snapshot["loopSamples"] or not snapshot["displaySamples"]):
        raise AssertionError("No loop or display timing samples after online tests")
    if snapshot["logEnabled"] and snapshot["logError"]:
        raise AssertionError("SD diagnostics log failed: " + snapshot["logError"])
    return "loop {} samples, average {} us, peak {} us; display {} samples, average {} us, peak {} us; SD log {}, build {} us, write {} us".format(
        snapshot["loopSamples"], snapshot["loopAverageUs"], snapshot["loopPeakUs"],
        snapshot["displaySamples"], snapshot["displayAverageUs"], snapshot["displayPeakUs"],
        "on" if snapshot["logEnabled"] else "off", snapshot["logBuildUs"], snapshot["logWriteUs"])


def valid_signal(lines):
    for line in lines:
        match = SIGNAL_PATTERN.search(line)
        if match:
            rms, peak, bias = map(int, match.groups())
            if 0 <= rms <= peak <= 4095 and 0 <= bias <= 4095:
                return rms, peak, bias
    raise AssertionError("No valid ADC signal sample: " + repr(lines))


def test_signal(device, report):
    state = menu_state(device)
    if state[4]:
        device.write(b"j")
        state = menu_state(device)
    while state[2] or state[3]:
        device.write(b"b")
        state = menu_state(device)
    original_tile = state[0]
    settings_tile = SETTINGS_TILE
    for _ in range((settings_tile - state[0]) % len(DISPLAY_TILES)):
        device.write(b"l")
        state = menu_state(device)
    if state[0] != settings_tile:
        raise AssertionError("Could not reach settings tile to enable serial ADC meter")
    device.write(b"k")
    state = menu_state(device)
    if not state[2]:
        raise AssertionError("Short press did not open settings")
    for _ in range((1 - state[1]) % SETTING_COUNT):
        device.write(b"l")
        state = menu_state(device)
    if state[1] != 1:
        raise AssertionError("Could not select serial ADC meter setting")
    device.write(b"k")
    meter_enabled = False
    try:
        lines = read_until(device, lambda line: "Serial ADC measurement enabled" in line,
                           timeout=3, report=report)
        meter_enabled = True
        lines.extend(read_until(device, lambda line: SIGNAL_PATTERN.search(line) is not None,
                                timeout=4, report=report))
        rms, peak, bias = valid_signal(lines)
        return "GPIO1 RMS={} peak={} bias={}".format(rms, peak, bias)
    finally:
        if meter_enabled:
            device.write(b"k")
            read_until(device, lambda line: "Serial ADC measurement disabled" in line,
                       timeout=3, report=report)
        state = menu_state(device)
        if state[2]:
            device.write(b"b")
            state = menu_state(device)
        for _ in range((original_tile - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)


def delete_test_recording(device, filename, report=None):
    if not re.fullmatch(r"/\d{4}-\d{2}-\d{2}/RadioMan\d{6}_\d{6}(?:_\d{2})?\.wav", filename):
        raise ValueError("Refusing non-recording filename: " + filename)
    device.write(b"x")
    read_until(device, lambda line: line.startswith("Delete test WAV:"), timeout=3, report=report)
    device.write(filename.encode("ascii") + b"\n")
    result = read_until(device, lambda line: line.startswith(("Test WAV deleted:", "Test WAV delete refused:")),
                        timeout=8, report=report)[-1]
    if result != "Test WAV deleted: " + filename:
        raise AssertionError("Firmware refused to delete test recording: " + result)
    return "removed " + filename


def test_recording(device, report, firmware_version):
    if not firmware_at_least(firmware_version, (0, 1, 43)):
        raise RuntimeError("record-and-delete requires firmware 0.1.43 or newer; found " + firmware_version)
    device.write(b"r")
    started = read_until(device, lambda line: line.startswith(("Recording started:", "Recording failed:",
                                                               "Cannot start recording")), timeout=5, report=report)
    if not started[-1].startswith("Recording started:"):
        raise AssertionError("Recording could not start: " + repr(started))
    start_line = next(line for line in started if line.startswith("Recording started:"))
    filename = start_line.split(":", 1)[1].strip().split(" ", 1)[0]
    try:
        time.sleep(1)
    finally:
        device.write(b"s")
    stopped = read_until(device, lambda line: line.startswith(("Recording stopped:", "Recording failed:")),
                         timeout=8, report=report)
    result = next(line for line in stopped if line.startswith(("Recording stopped:", "Recording failed:")))
    if not result.startswith("Recording stopped:") or filename not in result:
        raise AssertionError("Recording did not finalize successfully: " + result)
    try:
        errors = [line for line in stopped if line.startswith("Recording error:")]
        if errors:
            raise AssertionError("Recording reported errors: " + repr(errors))
        match = re.search(r"\((\d+) audio bytes\)", result)
        if not match or int(match.group(1)) <= 0:
            raise AssertionError("Recording contains no audio data: " + result)
    finally:
        delete_test_recording(device, filename, report)
    return result + "; test file deleted"


def test_radio_playback(device, report, path, firmware_version):
    if not firmware_at_least(firmware_version, (0, 1, 59)):
        raise RuntimeError("serial DAC playback requires firmware 0.1.59 or newer")
    encoded = path.encode("utf-8")
    if (not path.startswith("/") or not path.lower().endswith(".wav") or len(encoded) > 220 or
            any(segment in ("", ".", "..") for segment in path[1:].split("/")) or
            any(character in path for character in '@\\:\"\r\n\x00')):
        raise ValueError("Pass an absolute SD WAV path, for example /2026-10-28/example.wav")
    device.write(b"P" + path.encode("utf-8") + b"\n")
    start = read_until(device, lambda line: line.startswith("PLAYBACK_TEST accepted:") or
                       line.startswith("PLAYBACK_TEST refused:"), timeout=6, report=report)[-1]
    if start != "PLAYBACK_TEST accepted: " + path:
        raise AssertionError(start)
    try:
        result = read_until(device, lambda line: line.startswith(("Radio DAC first block:", "I2S radio write:")),
                            timeout=6, report=report)[-1]
        if not result.startswith("Radio DAC first block:"):
            raise AssertionError(result)
    finally:
        device.write(b"Z")
        read_until(device, lambda line: line.startswith("PLAYBACK_TEST "), report=report)
    device.write(b"i")
    status_lines = read_until(device, lambda line: line.startswith("Playback error:"), report=report)
    if status_lines[-1] != "Playback error:":
        raise AssertionError(status_lines[-1])
    return result + "; playback stopped; no SD write or PTT"


def list_radio_wavs(device, report, firmware_version):
    if not firmware_at_least(firmware_version, (0, 1, 60)):
        raise RuntimeError("serial WAV listing requires firmware 0.1.60 or newer")
    device.write(b"L")
    lines = read_until(device, lambda line: line.startswith(("WAV_LIST end", "WAV_LIST refused:")),
                       timeout=12, report=report)
    if not lines[-1].startswith("WAV_LIST end"):
        raise AssertionError(lines[-1])
    paths = [line.split(" (", 1)[0][5:] for line in lines if line.startswith("WAV: ") and " (PCM, " in line]
    if not paths:
        raise AssertionError("No supported PCM WAV on the SD card: " + lines[-1])
    return ", ".join(paths)


def menu_state(device):
    device.write(b"p")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = device.readline()
        if line.startswith(b"OLED_STATE "):
            values = tuple(int(value) for value in line.split()[1:])
            if len(values) != 5:
                raise AssertionError("Malformed OLED state")
            return values
    raise TimeoutError("No OLED menu state received")


def validate_display_frames(frames, expected_count, description):
    if len(frames) != expected_count:
        raise AssertionError("Expected {} {}, got {}".format(expected_count, description, len(frames)))
    signatures = []
    for index, frame in enumerate(frames):
        if len(frame) != 1024:
            raise AssertionError("{} {} has invalid frame length {}".format(description, index, len(frame)))
        body = body_pixels(frame)
        if not any(body):
            raise AssertionError("{} {} is blank".format(description, index))
        signatures.append(body)
    if len(set(signatures)) != expected_count:
        raise AssertionError("{} contain duplicate or indistinguishable views".format(description))
    return "{} distinct, non-blank {} views".format(expected_count, description)


def test_display_all(device, report):
    state = menu_state(device)
    for _ in range(2):
        if not state[2]:
            break
        device.write(b"b")
        state = menu_state(device)
    if state[2]:
        raise AssertionError("Could not exit settings before display test")
    original_tile = state[0]

    try:
        tile_frames = []
        for target, name in enumerate(DISPLAY_TILES):
            state = menu_state(device)
            for _ in range((target - state[0]) % len(DISPLAY_TILES)):
                device.write(b"l")
                state = menu_state(device)
            if state[0] != target or state[2]:
                raise AssertionError("Could not select main tile {} ({})".format(target, name))
            frame = read_frame(device)
            tile_frames.append(frame)
            report.detail("DISPLAY TILE {} {} bytes={}".format(target, name, len(frame)))
        validate_display_frames(tile_frames, len(DISPLAY_TILES), "main tiles")

        state = menu_state(device)
        for _ in range((SETTINGS_TILE - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        device.write(b"k")
        state = menu_state(device)
        if not state[2]:
            raise AssertionError("Settings tile did not open settings")
        setting_frames = []
        for target in range(SETTING_COUNT):
            state = menu_state(device)
            for _ in range((target - state[1]) % SETTING_COUNT):
                device.write(b"l")
                state = menu_state(device)
            if not state[2] or state[1] != target:
                raise AssertionError("Could not select setting row {}".format(target))
            frame = read_frame(device)
            setting_frames.append(frame)
            report.detail("DISPLAY SETTING {} bytes={}".format(target, len(frame)))
        validate_display_frames(setting_frames, SETTING_COUNT, "settings rows")
        device.write(b"b")
        state = menu_state(device)
        for _ in range((4 - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != 4 or state[2]:
            raise AssertionError("Could not reach player tile for browser test")
        device.write(b"k")
        browser_frame = read_frame(device)
        if browser_frame == tile_frames[4]:
            raise AssertionError("Player short press did not open a distinct SD browser")
        device.write(b"b")
        device.write(b"l")
        if menu_state(device)[0] != 5:
            raise AssertionError("Player browser long press did not restore tile navigation")
        report.detail("DISPLAY SD browser opened and exited without playback")
        return "all tiles/settings rendered; SD browser opens and exits"
    finally:
        state = menu_state(device)
        for _ in range(2):
            if not state[2]:
                break
            device.write(b"b")
            state = menu_state(device)
        if state[2]:
            raise AssertionError("Could not exit settings after display test")
        for _ in range((original_tile - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != original_tile:
            raise AssertionError("Could not restore original home tile")


def test_wake_input(device):
    state = menu_state(device)
    for _ in range(2):
        if not state[2]:
            break
        device.write(b"b")
        state = menu_state(device)
    if state[2]:
        raise AssertionError("Could not exit settings before wake test")
    original_tile = state[0]
    try:
        for _ in range((7 - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != 7:
            raise AssertionError("Could not reach safe Diagnostics tile")
        device.write(b"o")
        if not menu_state(device)[4]:
            raise AssertionError("Test sleep command did not put OLED to sleep")
        device.write(b"j")
        state = menu_state(device)
        if state[4] or state[0] != 7:
            raise AssertionError("First encoder input did not only wake OLED")
        device.write(b"l")
        state = menu_state(device)
        if state[0] != 8:
            raise AssertionError("Second encoder input did not navigate")
        for _ in range((SETTINGS_TILE - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != SETTINGS_TILE:
            raise AssertionError("Could not reach Settings tile for button wake test")

        device.write(b"o")
        if not menu_state(device)[4]:
            raise AssertionError("Test sleep command did not put OLED to sleep")
        device.write(b"k")
        state = menu_state(device)
        if state[4] or state[2] or state[0] != SETTINGS_TILE:
            raise AssertionError("First button input did more than wake OLED")
        device.write(b"k")
        state = menu_state(device)
        if not state[2]:
            raise AssertionError("Second button input did not open settings")
        device.write(b"b")
        if menu_state(device)[2]:
            raise AssertionError("Could not close settings after wake test")
        return "first encoder and button events wake only; second events act"
    finally:
        state = menu_state(device)
        if state[4]:
            device.write(b"j")
            state = menu_state(device)
        for _ in range(2):
            if not state[2]:
                break
            device.write(b"b")
            state = menu_state(device)
        for _ in range((original_tile - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != original_tile:
            raise AssertionError("Could not restore original tile after wake test")


def automatic_snapshot(device, report):
    device.write(b"a")
    lines = read_until(device, lambda line: line.startswith("AUTO_STATE "), report=report)
    return json.loads(lines[-1].removeprefix("AUTO_STATE "))


def test_automatic_settings(device, report):
    original = automatic_snapshot(device, report)
    if original["activeMode"] != "none" or any(original[source]["armed"] for source in ("radio", "microphone")):
        raise AssertionError("Automatic test requires inactive recording and arming")
    if any(original[source].get("blocked") for source in ("radio", "microphone")):
        raise AssertionError("Automatic test requires idle playback, live audio and file operations")
    original_navigation = menu_state(device)
    if original_navigation[2] or original_navigation[3]:
        raise AssertionError("Automatic settings test requires a non-editing home tile")
    original_tile = original_navigation[0]
    created = set()

    def snapshot():
        state = automatic_snapshot(device, report)
        for source in ("radio", "microphone"):
            path = state[source].get("lastFile", "")
            if path and path != original[source].get("lastFile", ""):
                created.add(path)
        if state.get("activeFile") and state["activeMode"] in ("autoRadio", "autoMic"):
            created.add(state["activeFile"])
        return state

    def select_tile(target):
        state = menu_state(device)
        while state[2] or state[3]:
            device.write(b"b"); state = menu_state(device)
        forward = (target - state[0]) % len(DISPLAY_TILES)
        backward = (state[0] - target) % len(DISPLAY_TILES)
        for _ in range(min(forward, backward)):
            device.write(b"l" if forward <= backward else b"j"); state = menu_state(device)

    def edit_step(row, direction):
        state = menu_state(device)
        if state[3]:
            device.write(b"b"); state = menu_state(device)
        if not state[2]:
            select_tile(SETTINGS_TILE); device.write(b"k"); state = menu_state(device)
        for _ in range((row - state[1]) % SETTING_COUNT):
            device.write(b"l")
        device.write(b"k")
        if not menu_state(device)[3]:
            raise AssertionError("Automatic setting did not enter editing")
        device.write(b"l" if direction > 0 else b"j")
        device.write(b"k")
        if menu_state(device)[3]:
            raise AssertionError("Automatic setting did not commit")

    fields = ((15, "radio", "threshold", 4095, 10), (16, "radio", "silenceSeconds", 60, 1),
              (17, "microphone", "threshold", 32768, 10), (18, "microphone", "silenceSeconds", 60, 1))
    try:
        for row, source, field, maximum, step in fields:
            value = original[source][field]
            direction = -1 if value + step > maximum else 1
            edit_step(row, direction)
            if snapshot()[source][field] != value + direction * step:
                raise AssertionError("Automatic setting did not change: " + field)
            edit_step(row, -direction)
            if snapshot()[source][field] != value:
                raise AssertionError("Automatic setting did not restore: " + field)
        edit_step(19, 1)
        order = snapshot()["priorities"]
        if order == original["priorities"] or len(set(order)) != 4:
            raise AssertionError("Priority edit did not produce a unique changed order")
        edit_step(19, -1)
        if snapshot()["priorities"] != original["priorities"]:
            raise AssertionError("Priority order did not restore")
        select_tile(2); device.write(b"k")
        if not snapshot()["radio"]["armed"]:
            raise AssertionError("Radio automatic arming failed")
        device.write(b"l"); device.write(b"k")
        state = snapshot()
        if not state["microphone"]["armed"] or state["activeMode"] != "none":
            raise AssertionError("Microphone arming or warm-up guard failed")
    finally:
        state = snapshot()
        for source, tile in (("radio", 2), ("microphone", 3)):
            if state[source]["armed"]:
                select_tile(tile); device.write(b"k"); state = snapshot()
        deadline = time.monotonic() + 5
        while state.get("activeFile") and state["activeMode"] in ("autoRadio", "autoMic"):
            if time.monotonic() >= deadline:
                raise TimeoutError("Automatic test WAV did not close")
            time.sleep(0.05); state = snapshot()
        state = snapshot()
        for row, source, field, _, step in fields:
            difference = original[source][field] - state[source][field]
            if difference % step:
                raise AssertionError("Cannot restore changed automatic setting exactly")
            for _ in range(abs(difference) // step):
                edit_step(row, 1 if difference > 0 else -1)
            state = snapshot()
        modes = ["manualMic", "manualRadio", "autoRadio", "autoMic"]
        for rank, desired in enumerate(original["priorities"]):
            for _ in range(4):
                state = snapshot()
                if state["priorities"][rank] == desired:
                    break
                difference = (modes.index(desired) - modes.index(state["priorities"][rank])) % 4
                edit_step(19 + rank, 1 if difference <= 2 else -1)
            if snapshot()["priorities"][rank] != desired:
                raise AssertionError("Automatic priority restoration failed")
        for path in created:
            delete_test_recording(device, path, report)
        select_tile(original_tile)
    return "thresholds, silence, unique priority and warm-up arming verified; settings/navigation restored"


def language_snapshot(device, report):
    device.write(b"i")
    lines = read_until(device, lambda line: line.startswith("Wi-Fi auto-off:"), report=report)
    language = next((line.split(": ", 1)[1] for line in lines if line.startswith("Language: ")), None)
    if language not in ("hu", "en"):
        raise AssertionError("Expected hu/en language status")
    return language


def test_language_selection(device, report):
    state = menu_state(device)
    original_tile = state[0]
    original_settings = bool(state[2])
    original_setting = state[1]
    original_language = language_snapshot(device, report)

    def select_language_row():
        state = menu_state(device)
        if not state[2]:
            for _ in range((SETTINGS_TILE - state[0]) % len(DISPLAY_TILES)):
                device.write(b"l")
            device.write(b"k")
        state = menu_state(device)
        if not state[2] or state[3]:
            raise AssertionError("Language test requires a non-editing settings menu")
        for _ in range((14 - state[1]) % SETTING_COUNT):
            device.write(b"l")

    try:
        select_language_row()
        before = read_frame(device)
        device.write(b"k")
        expected = "en" if original_language == "hu" else "hu"
        if language_snapshot(device, report) != expected:
            raise AssertionError("OLED language selection did not update the shared setting")
        if body_pixels(before) == body_pixels(read_frame(device)):
            raise AssertionError("OLED text did not change after language selection")
    finally:
        if language_snapshot(device, report) != original_language:
            select_language_row()
            device.write(b"k")
        if language_snapshot(device, report) != original_language:
            raise AssertionError("Could not restore original language")
        if original_settings:
            state = menu_state(device)
            for _ in range((original_setting - state[1]) % SETTING_COUNT):
                device.write(b"l")
        else:
            device.write(b"b")
            state = menu_state(device)
            for _ in range((original_tile - state[0]) % len(DISPLAY_TILES)):
                device.write(b"l")
    return "hu/en switch changed OLED text; original language and navigation restored"


def test_oled_navigation(device):
    for _ in range(2):
        if not menu_state(device)[2]:
            break
        device.write(b"b")
    original_tile = menu_state(device)[0]
    initial = read_frame(device)
    device.write(b"l")
    moved = read_frame(device)
    if menu_state(device)[0] != (original_tile + 1) % len(DISPLAY_TILES):
        raise AssertionError("Right encoder command did not advance tile")
    device.write(b"j")
    restored = read_frame(device)
    if menu_state(device)[0] != original_tile:
        raise AssertionError("Left encoder command did not restore tile")
    if body_pixels(initial) == body_pixels(moved):
        raise AssertionError("Right encoder command did not change the OLED body")
    if body_pixels(initial) != body_pixels(restored):
        raise AssertionError("Left encoder command did not restore the OLED body")
    settings_tile = SETTINGS_TILE
    for _ in range((settings_tile - original_tile) % len(DISPLAY_TILES)):
        device.write(b"l")
    if menu_state(device)[0] != settings_tile:
        raise AssertionError("Could not reach settings tile")
    device.write(b"k")
    if menu_state(device)[2] != 1:
        raise AssertionError("Short press did not open settings")
    device.write(b"b")
    if menu_state(device)[2] != 0:
        raise AssertionError("Long press did not close settings")
    for _ in range((original_tile - settings_tile) % len(DISPLAY_TILES)):
        device.write(b"l")
    if menu_state(device)[0] != original_tile:
        raise AssertionError("Could not restore original tile")
    return "frame, left/right, short/long press and menu restoration"


def main():
    parser = argparse.ArgumentParser(description="Check SD, ADC, OLED and serial encoder on RadioRecorder")
    parser.add_argument("--port", help="ESP serial port (default: auto-detect)")
    parser.add_argument("--record", action="store_true", help="Write and verify a brief WAV on the SD card")
    performance_mode = parser.add_mutually_exclusive_group()
    performance_mode.add_argument("--ensure-performance", action="store_true", help="Enable and verify diagnostics before online tests (NVS write only if disabled)")
    performance_mode.add_argument("--check-performance", action="store_true", help="Verify enabled diagnostics and timing samples after online tests (read-only)")
    parser.add_argument("--language-test", action="store_true", help="Opt in to hu/en language NVS writes, restoring the original selection")
    parser.add_argument("--auto-test", action="store_true", help="Opt in to restoring automatic settings/priority and brief warm-up arming checks")
    parser.add_argument("--play-radio", metavar="WAV_PATH", help="Opt in to DAC playback of an existing SD WAV, no PTT")
    parser.add_argument("--list-wav", action="store_true", help="List supported SD WAV paths over serial (read-only)")
    parser.add_argument("--display-only", action="store_true", help="Test every OLED tile and setting row")
    parser.add_argument("--rtc", action="store_true", help="Check RTC against elapsed time, AP-only without NTP")
    parser.add_argument("--rtc-reboot", action="store_true", help="Also reboot ESP and verify RTC recovery (opt-in)")
    args = parser.parse_args()
    if (args.ensure_performance or args.check_performance) and (args.record or args.language_test or args.auto_test or
            args.play_radio or args.list_wav or args.display_only or args.rtc or args.rtc_reboot):
        parser.error("--ensure-performance and --check-performance must run separately from device tests")
    if args.language_test and (args.play_radio or args.list_wav or args.rtc or args.rtc_reboot or args.display_only):
        parser.error("--language-test must run separately from playback, RTC, and display-only checks")
    if args.auto_test and (args.play_radio or args.list_wav or args.rtc or args.rtc_reboot or args.display_only):
        parser.error("--auto-test must run with the ordinary device check")
    if (args.play_radio or args.list_wav) and (args.record or args.rtc or args.rtc_reboot or args.display_only or
                                               args.play_radio and args.list_wav):
        parser.error("--play-radio and --list-wav must run separately from other device checks")
    if args.display_only and (args.rtc or args.rtc_reboot) or args.record and (args.rtc or args.rtc_reboot):
        parser.error("RTC tests cannot be combined with --record or --display-only")
    report = TestRun("rtc-reboot" if args.rtc_reboot else "rtc" if args.rtc else
                     "display" if args.display_only else "device")
    successful = False
    try:
        connected, device = report.case("ESP serial connection", lambda: find_device(args.port))
        if connected:
            report.detail("Connected to " + device.port)
            with device:
                if args.ensure_performance or args.check_performance:
                    report.case("Enable diagnostics before tests" if args.ensure_performance else "Verify diagnostics after tests",
                                lambda: performance_preflight(device, report) if args.ensure_performance else
                                performance_postcheck(device, report))
                elif args.play_radio or args.list_wav:
                    ready, firmware_version = report.case("SD and DAC status", lambda: status(device, report, radio_only=True))
                    if ready:
                        if args.list_wav:
                            report.case("SD WAV paths", lambda: list_radio_wavs(device, report, firmware_version))
                        else:
                            report.case("Radio DAC first I2S block", lambda: test_radio_playback(
                                device, report, args.play_radio, firmware_version))
                    else:
                        report.skip("SD WAV paths" if args.list_wav else "Radio DAC playback", "device status check failed")
                elif args.rtc or args.rtc_reboot:
                    ready, firmware_version = report.case("RTC test firmware", lambda: rtc_firmware_version(device, report))
                    if ready and firmware_at_least(firmware_version, (0, 1, 54)):
                        snapshot_ok, snapshot = report.case("RTC prerequisite snapshot", lambda: rtc_snapshot(device, report))
                        if snapshot_ok and not rtc_test_eligible(snapshot):
                            report.skip("RTC without NTP", "AP-only mode without saved STA credentials required")
                            if args.rtc_reboot:
                                report.skip("RTC after software restart", "AP-only prerequisite unavailable")
                        elif snapshot_ok:
                            passed, _ = report.case("RTC advances without NTP", lambda: rtc_live_check(device, report))
                            if args.rtc_reboot and passed:
                                report.case("RTC after software restart without NTP",
                                            lambda: rtc_live_check(device, report, reboot=True))
                            elif args.rtc_reboot:
                                report.skip("RTC after software restart", "read-only RTC check failed")
                        else:
                            report.skip("RTC without NTP", "RTC prerequisite snapshot failed")
                            if args.rtc_reboot:
                                report.skip("RTC after software restart", "RTC prerequisite snapshot failed")
                    else:
                        report.skip("RTC without NTP", "upload firmware 0.1.54 or newer")
                        if args.rtc_reboot:
                            report.skip("RTC after software restart", "RTC probe unavailable")
                else:
                    readiness = "OLED status" if args.display_only else "SD, ADC, recorder and OLED status"
                    ready, firmware_version = report.case(readiness,
                                                           lambda: status(device, report, display_only=args.display_only))
                    if ready:
                        if firmware_at_least(firmware_version, (0, 1, 90)):
                            report.case("Serial performance diagnostics (read-only)",
                                        lambda: test_performance_serial(device, report))
                        else:
                            report.skip("Serial performance diagnostics", "firmware 0.1.90+ required")
                        if args.display_only:
                            report.case("All OLED tiles and settings rows", lambda: test_display_all(device, report))
                            report.case("Wake consumes first encoder/button input", lambda: test_wake_input(device))
                        else:
                            signal_ok, _ = report.case("Live ADC sample", lambda: test_signal(device, report))
                            if signal_ok:
                                oled_ok, _ = report.case("OLED and encoder controls", lambda: test_oled_navigation(device))
                            else:
                                report.skip("OLED and encoder controls", "live ADC check failed")
                                oled_ok = False
                            if args.record and oled_ok:
                                report.case("Record, verify and delete test WAV",
                                            lambda: test_recording(device, report, firmware_version))
                            elif args.record:
                                report.skip("SD recording/deletion", "a previous check failed")
                            else:
                                report.skip("SD recording/deletion", "pass --record to opt in")
                            if args.language_test:
                                if oled_ok and firmware_at_least(firmware_version, (0, 1, 74)):
                                    report.case("OLED shared language selection and restoration",
                                                lambda: test_language_selection(device, report))
                                else:
                                    report.skip("OLED language selection", "requires ready OLED and firmware 0.1.74+")
                            if args.auto_test:
                                if oled_ok and firmware_at_least(firmware_version, (0, 1, 77)):
                                    report.case("Automatic settings, priority and arming restoration", lambda: test_automatic_settings(device, report))
                                else:
                                    report.skip("Automatic settings test", "requires ready OLED and firmware 0.1.77+")
                    else:
                        if args.display_only:
                            report.skip("All OLED tiles and settings rows", "OLED status check failed")
                        else:
                            report.skip("Live ADC sample", "device readiness check failed")
                            report.skip("OLED and encoder controls", "device readiness check failed")
                            report.skip("SD recording/deletion", "device readiness check failed")
        else:
            report.skip("RTC checks" if args.rtc or args.rtc_reboot else "Device checks", "ESP serial connection failed")
            if not (args.rtc or args.rtc_reboot):
                report.skip("SD recording/deletion", "device checks did not run")
    finally:
        successful = report.summary()
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())