# RadioRecorder project instructions

- Every feature or bug fix must include a focused new or updated test that would fail without the change. Run the relevant tests and the PlatformIO build before reporting completion; state explicitly when physical hardware or the Wi-Fi AP could not be exercised.
- Keep read-only serial/HTTP smoke tests the default. Recording, SD writes, Wi-Fi disruption, flashing, and other device-changing tests require explicit opt-in; never run them while another tool owns the COM port.
- PC-driven hardware and Wi-Fi tests must print a pass/fail/skip summary and save timestamped logs under the gitignored `.test-logs/` directory. A test-created SD recording must be removed after it is verified, using the exact filename created by that run.
- Follow `docs/testing.md` for offline, serial hardware, display, and Wi-Fi checks. Keep the OLED frame format and serial commands documented if they change.
- Increment `kFirmwareVersion` in `src/app_control.cpp` for firmware changes. Do not edit `firmware-build.txt` manually; the upload hook owns that counter.