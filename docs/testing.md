# Testing

## Offline

Install the PC serial dependency with `python -m pip install -r scripts/requirements-test.txt`. Tkinter is included with the Windows Python installation used for the viewer.

Run `python -m unittest discover -s test -p 'test_*.py' -v` and `pio run -e esp32-s3-devkitc-1`. Offline tests verify the 128x64 U8g2 framebuffer encoding, pixel addressing, and image comparisons. The build verifies the firmware; neither proves physical hardware behavior.

## Serial hardware

Upload the new firmware first (`pio run -e esp32-s3-devkitc-1 -t upload`). Only one program can open the COM port at a time: close the PlatformIO serial monitor before using the viewer, hardware tests, or upload. On this workstation the ESP appears as COM3; use `--port` for another port. The serial port is 115200 baud with DTR/RTS disabled.

Run `python scripts/test_device.py --port COM3` for a non-recording check. It queries live firmware/OLED status, fetches the actual OLED buffer, tests right/left movement, opens the settings tile with a short press, closes it with a long press, and restores the original home tile. Do not turn the physical encoder at the same time. It does not write to SD.

Run `python scripts/oled_viewer.py --port COM3` for a live PC image. The buttons and Left/Right/Enter/Escape keys emulate the encoder. "Kép mentése" saves a portable bitmap (PBM) screenshot. A short press can start or stop recording when the Felvetel tile is selected, just like the physical button.

Serial commands: `j` left, `l` right, `k` short press, `b` long press, `d` OLED frame, `p` OLED menu state. Existing commands remain `i` status, `r` recording start, `s` recording stop, `f` low-pass toggle. `d` returns `OLED_FRAME 128 64 ` plus 2048 uppercase hexadecimal digits and newline. The 1024 bytes use U8g2 vertical tiles: byte offset `(y // 8) * 128 + x`, bit `y % 8`. `p` returns `OLED_STATE tile setting inSettings editing`. The snapshot is on demand, so a client should poll `d` for animation. These commands are only available once the new firmware is uploaded.

## Wi-Fi AP

Connect the PC to the ESP's **RadioRecorder** Wi-Fi AP, then run `python scripts/test_wifi.py --base-url http://192.168.4.1`. On Windows, `python scripts/test_wifi.py --connect` can create the open-network profile and switch Wi-Fi automatically; this may disconnect the PC from its current network, and a VPN can still block access. Without `--connect`, the script does not change networking. The default HTTP test is read-only: it checks both pages, `/api/status`, and `/api/signal`. The PC currently uses a VPN that prevents connecting to the ESP AP, so this test cannot run here until network access is available.

When recording to SD is explicitly desired, run `python scripts/test_wifi.py --record`. It starts a brief WAV recording, stops it, and checks that the new file appears in `/api/recordings`. Do not run while another recording or a Wi-Fi noise test is in progress. This test cannot measure radio input quality, OLED contrast, I2C faults, or real encoder bounce; inspect the physical panel and listen to a WAV for those.