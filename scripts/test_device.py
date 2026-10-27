"""Serial hardware-in-the-loop checks; WAV recording is opt-in via --record."""

import argparse
import re
import time

from scripts.oled_viewer import body_pixels, find_device, read_frame


SIGNAL_PATTERN = re.compile(r"GPIO1 RMS: (\d+) \| Csucs: (\d+) \| DC-kozep: (\d+) \(ADC\)")


def read_until(device, predicate, timeout=3):
    lines = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            if predicate(line):
                return lines
    raise TimeoutError("Expected serial response not received: " + repr(lines))


def status(device):
    device.write(b"i")
    lines = []
    wifi_status_seen = False
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            wifi_status_seen |= line.startswith("Wi-Fi AP IP:") or line == "Wi-Fi AP: unavailable"
        elif wifi_status_seen:
            break
    if not wifi_status_seen:
        raise TimeoutError("Incomplete firmware status: " + repr(lines))
    if not any(line.startswith("Firmware: v") for line in lines):
        raise AssertionError("No firmware status from ESP")
    for label in ("SD card", "ADC GPIO1", "Recorder", "OLED"):
        if not any(line.startswith(label + ": ready") for line in lines):
            raise AssertionError(label + " is not ready: " + repr(lines))
    if any(line.startswith("Recording:") for line in lines):
        raise AssertionError("Stop the current recording before running device tests")
    print("\n".join(lines))
    return lines


def valid_signal(lines):
    for line in lines:
        match = SIGNAL_PATTERN.search(line)
        if match:
            rms, peak, bias = map(int, match.groups())
            if 0 <= rms <= peak <= 4095 and 0 <= bias <= 4095:
                return rms, peak, bias
    raise AssertionError("No valid ADC signal sample: " + repr(lines))


def test_signal(device):
    state = menu_state(device)
    while state[2] or state[3]:
        device.write(b"b")
        state = menu_state(device)
    original_tile = state[0]
    for _ in range((6 - state[0]) % 7):
        device.write(b"l")
        state = menu_state(device)
    if state[0] != 6:
        raise AssertionError("Could not reach settings tile to enable serial ADC meter")
    device.write(b"k")
    state = menu_state(device)
    if not state[2]:
        raise AssertionError("Short press did not open settings")
    for _ in range((1 - state[1]) % 6):
        device.write(b"l")
        state = menu_state(device)
    if state[1] != 1:
        raise AssertionError("Could not select serial ADC meter setting")
    device.write(b"k")
    meter_enabled = False
    try:
        lines = read_until(device, lambda line: "Soros ADC meres bekapcsolva" in line, timeout=3)
        meter_enabled = True
        lines.extend(read_until(device, lambda line: SIGNAL_PATTERN.search(line) is not None, timeout=4))
        rms, peak, bias = valid_signal(lines)
        print("PASS: ADC signal GPIO1 RMS={} peak={} bias={}".format(rms, peak, bias))
    finally:
        if meter_enabled:
            device.write(b"k")
            read_until(device, lambda line: "Soros ADC meres kikapcsolva" in line, timeout=3)
        state = menu_state(device)
        if state[2]:
            device.write(b"b")
            state = menu_state(device)
        for _ in range((original_tile - state[0]) % 7):
            device.write(b"l")
            state = menu_state(device)


def test_recording(device):
    device.write(b"r")
    started = read_until(device, lambda line: line.startswith(("Recording started:", "Recording failed:",
                                                               "Cannot start recording")), timeout=5)
    if not started[-1].startswith("Recording started:"):
        raise AssertionError("Recording could not start: " + repr(started))
    start_line = next(line for line in started if line.startswith("Recording started:"))
    filename = start_line.split(":", 1)[1].strip()
    time.sleep(1)
    device.write(b"s")
    stopped = read_until(device, lambda line: line.startswith(("Recording stopped:", "Recording failed:")), timeout=8)
    result = next(line for line in stopped if line.startswith(("Recording stopped:", "Recording failed:")))
    if not result.startswith("Recording stopped:") or filename not in result:
        raise AssertionError("Recording did not finalize successfully: " + result)
    match = re.search(r"\((\d+) audio bytes\)", result)
    if not match or int(match.group(1)) <= 0:
        raise AssertionError("Recording contains no audio data: " + result)
    errors = [line for line in stopped if line.startswith("Recording error:")]
    if errors:
        raise AssertionError("Recording reported errors: " + repr(errors))
    print("PASS: SD recording started and stopped: " + result)


def menu_state(device):
    device.write(b"p")
    deadline = time.monotonic() + 2.5
    while time.monotonic() < deadline:
        line = device.readline()
        if line.startswith(b"OLED_STATE "):
            values = tuple(int(value) for value in line.split()[1:])
            if len(values) != 4:
                raise AssertionError("Malformed OLED state")
            return values
    raise TimeoutError("No OLED menu state received")


def main():
    parser = argparse.ArgumentParser(description="Check SD, ADC, OLED and serial encoder on RadioRecorder")
    parser.add_argument("--port", help="ESP serial port (default: auto-detect)")
    parser.add_argument("--record", action="store_true", help="Write and verify a brief WAV on the SD card")
    args = parser.parse_args()
    with find_device(args.port) as device:
        print("Connected to " + device.port)
        status(device)
        print("PASS: SD card and recorder available")
        test_signal(device)
        for _ in range(2):
            if not menu_state(device)[2]:
                break
            device.write(b"b")
        original_tile = menu_state(device)[0]
        initial = read_frame(device)
        device.write(b"l")
        moved = read_frame(device)
        if menu_state(device)[0] != (original_tile + 1) % 7:
            raise AssertionError("Right encoder command did not advance tile")
        device.write(b"j")
        restored = read_frame(device)
        if menu_state(device)[0] != original_tile:
            raise AssertionError("Left encoder command did not restore tile")
        if body_pixels(initial) == body_pixels(moved):
            raise AssertionError("Right encoder command did not change the OLED body")
        if body_pixels(initial) != body_pixels(restored):
            raise AssertionError("Left encoder command did not restore the OLED body")
        for _ in range((6 - original_tile) % 7):
            device.write(b"l")
        if menu_state(device)[0] != 6:
            raise AssertionError("Could not reach settings tile")
        device.write(b"k")
        if menu_state(device)[2] != 1:
            raise AssertionError("Short press did not open settings")
        device.write(b"b")
        if menu_state(device)[2] != 0:
            raise AssertionError("Long press did not close settings")
        for _ in range((original_tile - 6) % 7):
            device.write(b"l")
        if menu_state(device)[0] != original_tile:
            raise AssertionError("Could not restore original tile")
        print("PASS: OLED frame, left/right, short/long press and menu restoration")
        if args.record:
            test_recording(device)


if __name__ == "__main__":
    main()