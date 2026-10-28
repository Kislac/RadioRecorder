"""Serial hardware-in-the-loop checks; WAV recording is opt-in via --record."""

import argparse
import re
import time

from scripts.oled_viewer import body_pixels, find_device, read_frame
from scripts.test_support import TestRun, firmware_at_least


SIGNAL_PATTERN = re.compile(r"GPIO1 RMS: (\d+) \| Csucs: (\d+) \| DC-kozep: (\d+) \(ADC\)")
DISPLAY_TILES = ("Felvetel", "Mikrofon", "Lejatszo", "Elo jel", "Diagnosztika", "Radio", "Wi-Fi", "Beallitasok")
SETTING_COUNT = 8


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


def status(device, report, display_only=False):
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
    required = ("OLED",) if display_only else ("SD card", "ADC GPIO1", "Recorder", "OLED", "Speaker")
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
    return next(line.split(": ", 1)[1] for line in lines if line.startswith("Firmware: v"))


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
    settings_tile = len(DISPLAY_TILES) - 1
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
        lines = read_until(device, lambda line: "Soros ADC meres bekapcsolva" in line,
                           timeout=3, report=report)
        meter_enabled = True
        lines.extend(read_until(device, lambda line: SIGNAL_PATTERN.search(line) is not None,
                                timeout=4, report=report))
        rms, peak, bias = valid_signal(lines)
        return "GPIO1 RMS={} peak={} bias={}".format(rms, peak, bias)
    finally:
        if meter_enabled:
            device.write(b"k")
            read_until(device, lambda line: "Soros ADC meres kikapcsolva" in line,
                       timeout=3, report=report)
        state = menu_state(device)
        if state[2]:
            device.write(b"b")
            state = menu_state(device)
        for _ in range((original_tile - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)


def delete_test_recording(device, filename, report=None):
    if not re.fullmatch(r"REC\d{5}\.wav", filename):
        raise ValueError("Refusing non-recording filename: " + filename)
    device.write(b"x")
    read_until(device, lambda line: line.startswith("Delete test WAV:"), timeout=3, report=report)
    device.write(filename.encode("ascii") + b"\n")
    result = read_until(device, lambda line: line.startswith(("Test WAV deleted:", "Test WAV delete refused:")),
                        timeout=3, report=report)[-1]
    if result != "Test WAV deleted: " + filename:
        raise AssertionError("Firmware refused to delete test recording: " + result)
    return "removed " + filename


def test_recording(device, report, firmware_version):
    if not firmware_at_least(firmware_version, (0, 1, 26)):
        raise RuntimeError("record-and-delete requires firmware 0.1.26 or newer; found " + firmware_version)
    device.write(b"r")
    started = read_until(device, lambda line: line.startswith(("Recording started:", "Recording failed:",
                                                               "Cannot start recording")), timeout=5, report=report)
    if not started[-1].startswith("Recording started:"):
        raise AssertionError("Recording could not start: " + repr(started))
    start_line = next(line for line in started if line.startswith("Recording started:"))
    filename = start_line.split(":", 1)[1].strip()
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


def menu_state(device):
    device.write(b"p")
    deadline = time.monotonic() + 2.5
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
        return "all main tiles and settings rows rendered distinctly"
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
        for _ in range((5 - state[0]) % len(DISPLAY_TILES)):
            device.write(b"l")
            state = menu_state(device)
        if state[0] != 5:
            raise AssertionError("Could not reach safe Radio tile")
        device.write(b"o")
        if not menu_state(device)[4]:
            raise AssertionError("Test sleep command did not put OLED to sleep")
        device.write(b"j")
        state = menu_state(device)
        if state[4] or state[0] != 5:
            raise AssertionError("First encoder input did not only wake OLED")
        device.write(b"l")
        state = menu_state(device)
        if state[0] != 6:
            raise AssertionError("Second encoder input did not navigate")
        device.write(b"l")
        state = menu_state(device)
        if state[0] != len(DISPLAY_TILES) - 1:
            raise AssertionError("Could not reach Settings tile for button wake test")

        device.write(b"o")
        if not menu_state(device)[4]:
            raise AssertionError("Test sleep command did not put OLED to sleep")
        device.write(b"k")
        state = menu_state(device)
        if state[4] or state[2] or state[0] != len(DISPLAY_TILES) - 1:
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
    settings_tile = len(DISPLAY_TILES) - 1
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
    parser.add_argument("--display-only", action="store_true", help="Test every OLED tile and setting row")
    args = parser.parse_args()
    report = TestRun("display" if args.display_only else "device")
    successful = False
    try:
        connected, device = report.case("ESP serial connection", lambda: find_device(args.port))
        if connected:
            report.detail("Connected to " + device.port)
            with device:
                readiness = "OLED status" if args.display_only else "SD, ADC, recorder and OLED status"
                ready, firmware_version = report.case(readiness,
                                                       lambda: status(device, report, display_only=args.display_only))
                if ready:
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
                else:
                    if args.display_only:
                        report.skip("All OLED tiles and settings rows", "OLED status check failed")
                    else:
                        report.skip("Live ADC sample", "device readiness check failed")
                        report.skip("OLED and encoder controls", "device readiness check failed")
                        report.skip("SD recording/deletion", "device readiness check failed")
        else:
            report.skip("Device checks", "ESP serial connection failed")
            report.skip("SD recording/deletion", "device checks did not run")
    finally:
        successful = report.summary()
    return 0 if successful else 1


if __name__ == "__main__":
    main()