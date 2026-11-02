# Automatic recording

Firmware 0.1.77 adds independent automatic radio and microphone arming with **one WAV recording at a time**. Parallel file recording is not enabled. Both inputs may be measured to arbitrate requests, but only the selected source is written.

## Settings

| Source | RMS threshold | Allowed threshold | Silence time |
| --- | --- | --- | --- |
| Radio ADC | 100 | 1–4095 ADC units | 3 seconds, adjustable 1–60 |
| I2S microphone | 100 | 1–32768 PCM units | 3 seconds, adjustable 1–60 |

RMS is measured in 100 ms windows after DC removal on the ADC. Microphone RMS uses signed 16-bit PCM. The same number does not represent the same physical volume on both inputs. Use the displayed live RMS and adjust each threshold independently.

All four values and the complete priority permutation are stored together in flash NVS, not the DS3231 companion EEPROM. Values survive restart; **arming does not**. Boot always leaves both automatic modes inactive. Invalid settings are rejected before changing live state; failed NVS writes leave the previous configuration active.

Web Settings has four number inputs and four priority selectors. Changing one priority swaps the displaced mode, so each mode appears exactly once. Save commits the entire configuration and does not arm recording.

OLED Settings rows 15–18 (zero-based) edit radio RMS, radio silence, mic RMS, mic silence. Press enters editing, rotate changes the draft, press saves, long press discards. RMS steps by 10, silence by one second; the web offers single-unit RMS precision. Rows 19–22 edit priorities using swaps; row 23 is Back. Language stays at row 14. Settings remains the last home tile.

## Detection and files

Press the Auto Radio or Auto Mic home tile to toggle arming; web home has separate Active switches. Arming is independent of page visibility, encoder navigation and OLED sleep. Monitoring is continuous while resources are available; it does not depend on the signal-page four-second polling lifetime.

After arming/resuming, a two-second settling period rejects startup transients. Two consecutive 100 ms windows at or above the threshold request recording. A chronological 500 ms PCM ring buffer is written before the new samples so the onset is retained. During recording, sound at or above 80% of the threshold resets the silence timer. Continuous quiet for the selected time closes the WAV normally, including that quiet tail. Arming remains active and a later threshold event starts a new file.

Files use local date folders and distinct prefixes: `RadioAutYYMMDD_HHMMSS.wav` and `MicAutYYMMDD_HHMMSS.wav`; manual names remain `RadioMan...`/`MicMan...`. Same-second collisions get the existing numbered suffix, never overwrite. Displayed recording duration is derived from successfully written PCM bytes, including prebuffer and quiet tail, not wall-clock elapsed time. The previous finalized auto file and duration are retained separately per source until reboot.

## Priorities and stopping

Default highest-to-lowest order:

1. Manual Mic
2. Manual Radio
3. Auto Radio
4. Auto Mic

A higher-priority eligible request first requests stop, waits for WAV finalization, then starts its own file. There is never a second active file. A lower-priority manual request is refused, not queued for unexpected later recording. An accepted higher-priority manual request cannot be replaced by a later lower-priority one during close. Interrupted manual sessions end; they do not resume automatically. Armed automatic sources continue monitoring and may request a new file from fresh signal after the higher-priority session ends. Custom priority order also applies when an automatic mode outranks a manual mode.

The manual tiles and web buttons act on their own manual mode: press again stops its recording; pressing while another mode owns the recorder requests that manual mode subject to priorities. The common stop API/serial `s` clears pending manual requests; if it stops an automatic file it also disarms that owner to avoid immediate restart. The other automatic source remains armed. To stop all automatic recording, turn off both Active switches.

## Display and safety

Each auto tile uses four lines: Active ON/OFF with right-aligned live RMS, threshold/silence settings, recording/wait/error state, and current PCM duration. RMS displays `--` when inactive, blocked or in error rather than presenting a retained value as a live measurement. When idle after a completed file the last filename occupies the final line. Web home additionally shows blocking/error details, last-file path and duration. Manual tiles also display PCM duration.

Firmware 0.1.78 resets the acquisition timeout baseline after a finalized automatic WAV. A long valid recording therefore stays armed while fresh monitoring samples resume; genuine missing/stale input still produces an error and disarms that source.

Playback/file jobs suspend automatic acquisition requests without changing arming; monitoring settles again when resources return. Browser live audio cannot start while either automatic mode is armed. Blocking direct-file/legacy ZIP downloads are refused while armed; disarm first or use the managed ZIP job, which suspends monitoring. ADC raw probes, watchdog reset and RTC reboot guards also refuse while armed. Recording start/stop sound cues are suppressed for automatic recordings and whenever automatic monitoring is armed to prevent acoustic self-triggering.

Input read/overflow/SD errors are reported and affected auto modes disarm instead of repeatedly creating broken files. A source producing no samples after five seconds, or stale samples for over two seconds, also disarms with an error. Re-arming clears the previous source error. Reaching the WAV size limit closes/fails safely and requires re-arming.

## API and tests

`GET /api/auto-record` returns `radio`, `microphone`, `priorities`, `activeMode`, `activeFile`. Per-source status includes `armed`, `recording`, `rms`, `threshold`, `silenceSeconds`, `elapsedMs`, `lastElapsedMs`, `lastFile`, `blocked`, `error`. `/api/status` embeds this as `automatic`, plus `recordingMode` and `recordElapsedMs`.

`POST /api/auto-record/arm?source=radio&enabled=1` arms; `source=microphone`, `enabled=0` disarms. `POST /api/auto-record/settings` accepts JSON:

```json
{"radio":{"threshold":100,"silenceSeconds":3},"microphone":{"threshold":100,"silenceSeconds":3},"priorities":["manualMic","manualRadio","autoRadio","autoMic"]}
```

Manual starts return 200 when started, 202 while waiting for the previous WAV to close, and 409 when blocked. Serial `a` is read-only and returns `AUTO_STATE ` followed by the same JSON and newline. Existing serial recording commands remain available.

Focused offline checks: `python -m unittest test.test_auto_record -v`. They run actual C++ coordinator logic against clock/recorder/NVS fakes, detector/hysteresis/silence boundaries, stale input, pending requests, custom priorities, failed persistence, prebuffer wrap/handoff, translated OLED geometry and deterministic PCM preview files.

`python -m scripts.test_device --port COM3 --auto-test` explicitly opts in to temporary NVS changes and brief arming of both inputs during warm-up, restores all four settings/priorities/navigation, and cleans any exact run-created automatic WAV. It refuses already armed/recording devices. The standard authorized online suite includes it; read-only/device display tests never arm automatic recording. Physical threshold/quiet-tail quality with real radio/microphone signals still requires a controlled safe audio source and must be reported separately from synthetic tests.