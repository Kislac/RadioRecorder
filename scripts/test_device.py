"""Non-destructive serial hardware-in-the-loop test for the OLED and encoder."""

import argparse
import time

from oled_viewer import body_pixels, open_device, read_frame


def status(device):
    device.write(b"i")
    lines = []
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
        if line.startswith("Wi-Fi AP IP:") or line == "Wi-Fi AP: unavailable":
            break
    if not any(line.startswith("Firmware: v") for line in lines):
        raise AssertionError("No firmware status from ESP")
    if "OLED: ready" not in lines:
        raise AssertionError("OLED not ready: " + repr(lines))
    print("\n".join(lines))


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
    parser = argparse.ArgumentParser(description="Check real OLED frames and serial encoder movement")
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()
    with open_device(args.port) as device:
        status(device)
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


if __name__ == "__main__":
    main()