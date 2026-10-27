# Testing

## Offline

Install the PC serial dependency with `python -m pip install -r scripts/requirements-test.txt`. Tkinter is included with the Windows Python installation used for the viewer.

Run `python -m unittest discover -s test -p 'test_*.py' -v` and `pio run -e esp32-s3-devkitc-1`. Offline tests verify the 128x64 U8g2 framebuffer encoding, pixel addressing, and image comparisons. The build verifies the firmware; neither proves physical hardware behavior.

PC-driven hardware and Wi-Fi tests print a pass/fail/skip summary and write timestamped details to `.test-logs/` in the workspace root. Git ignores this directory.

## Serial hardware

Upload the new firmware first (`pio run -e esp32-s3-devkitc-1 -t upload`). Only one program can open the COM port at a time: close the PlatformIO serial monitor before using the viewer, hardware tests, or upload. On this workstation the ESP appears as COM3; use `--port` for another port. The serial port is 115200 baud with DTR/RTS disabled.

Run `python -m scripts.test_device` for a non-recording check. It scans serial ports, waits up to about one second per port for a RadioRecorder status response, then keeps that port open. It verifies SD, ADC, recorder, and OLED readiness, enables serial ADC metering and validates RMS/peak/DC ranges, fetches the actual OLED buffer, tests right/left movement, opens settings with a short press, closes it with a long press, and restores the original home tile. Do not turn the physical encoder at the same time. The default does not write to SD. Use `--port COM8` only to select a known port directly.

Run `python -m scripts.test_device --record` only when an SD write is intended. Firmware 0.1.26 or newer is required. In addition to the checks above, it starts a short recording, requests stop, verifies a finalized WAV with non-zero audio data, then deletes that exact test-created file and confirms removal. This is destructive test data on the card; use it only with the correct device and an available SD card. The firmware accepts only an exact valid `RECxxxxx.wav` filename and refuses deletion while recording; arbitrary paths and other file types cannot be deleted with this command.

Run `python -m scripts.test_device --display-only` for the display-only test. It checks OLED readiness, captures every home tile and each of the six settings rows, rejects blank or visually duplicate views, and restores the original tile without toggling settings or writing to SD.

VS Code's **Terminal > Run Task** menu includes `RadioRecorder: Display Test` and `RadioRecorder: All Tests (read-only)`, as well as `RadioRecorder: Device Test (read-only)`, `RadioRecorder: Device Test (record to SD)`, `RadioRecorder: OLED Viewer`, `RadioRecorder: Wi-Fi API Test (read-only)`, `RadioRecorder: Offline Tests`, and `RadioRecorder: Firmware Build`. The aggregate task runs offline tests, build, serial device test, display test, and Wi-Fi read-only API test sequentially. It does not upload firmware or record to SD; its device tasks require the test firmware already be uploaded and the serial monitor closed. The Wi-Fi test requires the PC to reach the ESP AP. The recording task is explicitly write-enabled; other test tasks are read-only apart from menu navigation and briefly enabling ADC measurement.

Run `python -m scripts.oled_viewer` for a live PC image; it uses the same serial port discovery. Left/Right emulates rotation, Enter a short press, and Space (or Escape) a long press; a brief Space tap sends one long-press event. "Kép mentése" saves a portable bitmap (PBM) screenshot. A short press can start or stop recording when the Felvetel tile is selected, just like the physical button.

Serial commands: `j` left, `l` right, `k` short press, `b` long press, `d` OLED frame, `p` OLED menu state, `m` serial ADC meter toggle, and `x` followed by an exact `RECxxxxx.wav` filename and newline to delete that recording. Existing commands remain `i` status, `r` recording start, `s` recording stop, `f` low-pass toggle. `d` returns `OLED_FRAME 128 64 ` plus 2048 uppercase hexadecimal digits and newline. The 1024 bytes use U8g2 vertical tiles: byte offset `(y // 8) * 128 + x`, bit `y % 8`. `p` returns `OLED_STATE tile setting inSettings editing`. The snapshot is on demand, so a client should poll `d` for animation. These commands are only available once the new firmware is uploaded.

## Wi-Fi AP

Connect the PC to the ESP's **RadioRecorder** Wi-Fi AP, then run `python -m scripts.test_wifi --base-url http://192.168.4.1`. On Windows, `python -m scripts.test_wifi --connect` can create the open-network profile and switch Wi-Fi automatically; this may disconnect the PC from its current network, and a VPN can still block access. Without `--connect`, the script does not change networking. The default HTTP test is read-only: it checks both pages, `/api/status`, and `/api/signal`. The PC currently uses a VPN that prevents connecting to the ESP AP, so this test cannot run here until network access is available.

When recording to SD is explicitly desired, run `python -m scripts.test_wifi --record`. It requires firmware 0.1.26+, an idle recorder, and the USB serial connection for safe cleanup. It starts a brief WAV, stops it, confirms the file appears in `/api/recordings`, then deletes that exact file over the guarded serial command. Do not run while another recording or a Wi-Fi noise test is in progress. This test cannot measure radio input quality, OLED contrast, I2C faults, or real encoder bounce; inspect the physical panel and listen to a WAV for those.