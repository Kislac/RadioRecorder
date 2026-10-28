# Testing

## Offline

ESP nélküli webes tervezéshez használd a `RadioRecorder: Web Preview (offline)` taskot. A külön HTML/CSS/JS forrásokat és a szimulált API-t a [webes fejlesztési útmutató](web-development.md) ismerteti. A standard offline tesztcsomag localhost HTTP-tesztekkel ellenőrzi ezt; nincs szüksége csatlakoztatott eszközre.

Install the PC serial dependency with `python -m pip install -r scripts/requirements-test.txt`. Tkinter is included with the Windows Python installation used for the viewer.

Run `python -m unittest discover -s test -p 'test_*.py' -v` and `pio run -e esp32-s3-devkitc-1`. Offline tests verify the 128x64 U8g2 framebuffer encoding, pixel addressing, and image comparisons. The build verifies the firmware; neither proves physical hardware behavior.

The firmware enables a 10-second ESP-IDF Task Watchdog. It monitors both core idle tasks and the Arduino loop; recorder and speaker workers are subscribed only while active and feed it as they make progress. A non-yielding infinite loop or a stalled active worker therefore triggers panic/reboot. A watchdog reset is not itself proof that the original fault is fixed; inspect the reset log and reproduce the triggering workload.

PC-driven hardware and Wi-Fi tests print a pass/fail/skip summary and write timestamped details to `.test-logs/` in the workspace root. Git ignores this directory.

The ESP-IDF Task Watchdog uses a 10-second timeout, monitors both core idle tasks, the Arduino main loop, and active recorder/speaker work. Normal workers blocked on notifications are not subscribed. A detected stall triggers panic/reset. `RadioRecorder: Watchdog Reset Test (reboots ESP)` is intentionally excluded from `All Tests`: it sends serial command `w`, freezes the main loop, and verifies a watchdog panic followed by firmware reboot. This test interrupts the current ESP session and should only be run when an automatic reset is acceptable.

## Serial hardware

Upload the new firmware first (`pio run -e esp32-s3-devkitc-1 -t upload`). Only one program can open the COM port at a time: close the PlatformIO serial monitor before using the viewer, hardware tests, or upload. On this workstation the ESP appears as COM3; use `--port` for another port. The serial port is 115200 baud with DTR/RTS disabled.

Run `python -m scripts.test_device` for a non-recording check. It scans serial ports, waits up to about one second per port for a RadioRecorder status response, then keeps that port open. It verifies SD, ADC, recorder, and OLED readiness, enables serial ADC metering and validates RMS/peak/DC ranges, fetches the actual OLED buffer, tests right/left movement, opens settings with a short press, closes it with a long press, and restores the original home tile. Do not turn the physical encoder at the same time. The default does not write to SD. Use `--port COM8` only to select a known port directly.

Run `python -m scripts.test_device --record` only when an SD write is intended. Firmware 0.1.26 or newer is required. In addition to the checks above, it starts a short recording, requests stop, verifies a finalized WAV with non-zero audio data, then deletes that exact test-created file and confirms removal. This is destructive test data on the card; use it only with the correct device and an available SD card. The firmware accepts only an exact valid `RECxxxxx.wav` filename and refuses deletion while recording; arbitrary paths and other file types cannot be deleted with this command.

Run `python -m scripts.test_device --display-only` for the display-only test. It checks OLED readiness, captures every home tile and each of the eight settings rows, rejects blank or visually duplicate views, and restores the original tile without toggling settings or writing to SD.

## Screen timeout

The OLED shows the previous project's built-in 128x64 ClearSky splash during boot, then returns to the home screen. A short firmware-generated startup melody plays through the I2S speaker; no SD file is read. Successful recording start and stop events play separate short melodies through the same firmware-generated path.

Serial status and the OLED **Diagnosztika** tile show both firmware version/build number and the compiler's embedded `__DATE__`/`__TIME__` timestamp. The timestamp is the compilation time; upload builds increment the firmware build number through the existing upload hook.

The OLED timeout defaults to 5 minutes. The encoder settings include `Kijelzo ido`: turn to set 0–999 minutes, press to finish editing; 0 means never turn off. When asleep, the first encoder movement or button press only wakes and redraws the display; that input is consumed and does not navigate or activate a tile. The next input performs its normal action. The MCU and recorder keep running while the panel is in U8g2 power-save; this saves display power only, not the ESP32/Wi-Fi/ADC baseline.

## Wi-Fi modes

With no saved external network, boot starts the open `RadioRecorder` AP. Saved credentials trigger a 15-second station connection attempt while the AP stays available; failure falls back to AP. On successful join, `Keep AP host` (unchecked by default) selects AP+STA; otherwise the device switches to STA-only. The password is stored in ESP32 NVS and is never returned by status endpoints. The `/settings` page can scan, select an SSID, enter a password, connect, and forget the network. `GET /api/wifi/status` reports the current mode, IP, saved SSID, Keep AP Host choice and timeout without returning the password. `POST /api/wifi/scan` starts a scan and `GET /api/wifi/scan` polls results; `POST /api/wifi/connect` takes `ssid`, `password` and `keepApHost`; `POST /api/wifi/forget` clears credentials and starts the open AP. The Wi-Fi home tile shows mode/SSID/IP and its short press forgets saved credentials; Wi-Fi auto-off is also on the OLED settings list.

Wi-Fi auto-off defaults to 5 minutes; 0 disables it. Encoder and web request activity reset the timer. The radio remains enabled during recording, live audio, scans, or a connection attempt. If STA-only Wi-Fi times out, an encoder action can re-enable Wi-Fi; the normal saved-network attempt runs again with AP fallback. After forget, the AP starts immediately. The AP is open, so use only a trusted environment.

The main web page links to `/settings`; that page links back to `/`. `GET /api/settings` returns `screenOffMinutes`; `POST /api/settings?screenOffMinutes=N` validates and saves the 0–999 value. Values are stored in ESP32 NVS via `Preferences` and loaded on boot. The settings page has a **Kijelző felébresztése** button backed by `POST /api/display/wake`; saving a timeout also wakes the panel. The Wi-Fi AP is open, so only use its setting route on a trusted test network.

VS Code's **Terminal > Run Task** menu includes `RadioRecorder: Display Test` and `RadioRecorder: All Tests (read-only)`, as well as `RadioRecorder: Device Test (read-only)`, `RadioRecorder: Device Test (record to SD)`, `RadioRecorder: OLED Viewer`, `RadioRecorder: Wi-Fi API Test (read-only)`, `RadioRecorder: Offline Tests`, and `RadioRecorder: Firmware Build`. The aggregate task runs offline tests, build, serial device test, display test, and Wi-Fi read-only API test sequentially. It does not upload firmware or record to SD; its device tasks require the test firmware already be uploaded and the serial monitor closed. The Wi-Fi test requires the PC to reach the ESP's current AP or STA IP. The recording task is explicitly write-enabled; other test tasks are read-only apart from menu navigation and briefly enabling ADC measurement.

`RadioRecorder: Watchdog Reset Test (reboots ESP)` is intentionally not part of `All Tests`: it sends serial command `w`, deliberately blocks the main loop, and verifies the 10-second watchdog panic and reboot. Use it only when resetting the ESP is acceptable; it does not write to SD.

Run `python -m scripts.oled_viewer` for a live PC image; it uses the same serial port discovery. Left/Right emulates rotation, Enter a short press, and Space (or Escape) a long press; a brief Space tap sends one long-press event. "Kép mentése" saves a portable bitmap (PBM) screenshot. A short press can start or stop recording when the Felvetel tile is selected, just like the physical button.

Serial commands: `j` left, `l` right, `k` short press, `b` long press, `d` OLED frame, `p` OLED menu state, `o` force OLED sleep for wake testing, `w` deliberately stall the main loop for the watchdog reset test, `m` serial ADC meter toggle, and `x` followed by an exact `RECxxxxx.wav` filename and newline to delete that recording. Existing commands remain `i` status, `r` recording start, `s` recording stop, `f` low-pass toggle. `d` returns `OLED_FRAME 128 64 ` plus 2048 uppercase hexadecimal digits and newline. The 1024 bytes use U8g2 vertical tiles: byte offset `(y // 8) * 128 + x`, bit `y % 8`. `p` returns `OLED_STATE tile setting inSettings editing screenSleeping`. The snapshot is on demand, so a client should poll `d` for animation. These commands are only available once the new firmware is uploaded.

## Wi-Fi AP

Connect the PC to the ESP's **RadioRecorder** Wi-Fi AP, then run `python -m scripts.test_wifi --base-url http://192.168.4.1`. On Windows, `python -m scripts.test_wifi --connect` can create the open-network profile and switch Wi-Fi automatically; this may disconnect the PC from its current network, and a VPN can still block access. Without `--connect`, the script does not change networking. The default HTTP test is read-only: it checks both pages, `/api/status`, and `/api/signal`. The PC currently uses a VPN that prevents connecting to the ESP AP, so this test cannot run here until network access is available.

When recording to SD is explicitly desired, run `python -m scripts.test_wifi --record`. It requires firmware 0.1.26+, an idle recorder, and the USB serial connection for safe cleanup. It starts a brief WAV, stops it, confirms the file appears in `/api/recordings`, then deletes that exact file over the guarded serial command. Do not run while another recording or a Wi-Fi noise test is in progress. This test cannot measure radio input quality, OLED contrast, I2C faults, or real encoder bounce; inspect the physical panel and listen to a WAV for those.