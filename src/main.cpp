#include <Arduino.h>
#include "playback.h"
#include "live_audio.h"
#include "recorder.h"
#include "app_control.h"
#include "web_server.h"
#include "controls.h"
#include "display.h"
#include "watchdog.h"
#include "wifi_manager.h"
#include "ptt_test.h"
#include "radio_programming.h"
#include "radio_serial.h"
#include "wav.h"
#include <SD_MMC.h>

void printSetupStatus() {
  Serial.printf("Firmware: v%s\n", appFirmwareVersion().c_str());
  Serial.printf("Build time: %s\n", appFirmwareBuildDateTime().c_str());
  Serial.println("Watchdog: active (10 s, both cores)");
  Serial.printf("3.4 kHz low-pass: %s (f = toggle)\n", recorderLowpass() ? "on" : "off");
  Serial.printf("OLED screen off: %u min\n", appScreenOffMinutes());
  PlaybackStatus playback = playbackStatus();
  Serial.printf("Speaker: %s\n", playback.ready ? "ready" : "unavailable");
  Serial.printf("Radio DAC GPIO12/13/15: %s\n", playback.radioReady ? "ready" : "unavailable");
  Serial.printf("Playback: %s\n", playback.state == PlaybackState::idle ? "idle" :
                playback.state == PlaybackState::starting ? "starting" : "playing");
  Serial.printf("Playback target: %s\n", playback.target == PlaybackTarget::radio ? "radio" : "speaker");
  Serial.printf("Playback path: %s\n", playback.path.c_str());
  Serial.printf("Playback error: %s\n", playback.error.c_str());
  Serial.printf("SD card: %s\n", appSdReady() ? "ready" : "unavailable");
  Serial.printf("ADC GPIO%d: %s\n", kAudioGpio, recorderAdcReady() ? "ready" : "unavailable");
  Serial.printf("I2S microphone GPIO6/7/8: %s\n", recorderMicReady() ? "initialized" : "unavailable");
  Serial.printf("Recorder: %s\n", appSdReady() && recorderTaskReady() ? "ready" : "unavailable");
  Serial.printf("OLED: %s\n", displayReady() ? "ready" : "unavailable (check GPIO4/5, 3V3, GND and I2C address)");
  Serial.printf("RTC DS3231 GPIO4/5: %s\n", appRtcStatus());
  Serial.printf("Wi-Fi mode: %s\n", wifiManagerModeName().c_str());
  Serial.printf("Wi-Fi SSID: %s\n", wifiManagerSSID().c_str());
  Serial.printf("Wi-Fi IP: %s\n", wifiManagerIPAddress().c_str());
  Serial.printf("Wi-Fi auto-off: %u min\n", wifiManagerAutoOffMinutes());
  if (recorderState() != CaptureState::idle) Serial.printf("Recording: %s\n", recorderName());
}

static void listSerialWavs(const String &path, uint8_t depth, uint16_t &remaining) {
  File directory = SD_MMC.open(path, FILE_READ);
  if (!directory || !directory.isDirectory()) return;
  File entry;
  while (remaining && (entry = directory.openNextFile())) {
    --remaining;
    String name = entry.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    String childPath = path == "/" ? "/" + name : path + "/" + name;
    if (safeSdPath(childPath)) {
      if (entry.isDirectory() && depth < 3) {
        listSerialWavs(childPath, depth + 1, remaining);
      } else if (!entry.isDirectory()) {
        String lowerName = name;
        lowerName.toLowerCase();
        if (lowerName.endsWith(".wav")) {
          uint32_t pcmBytes = 0;
          bool supported = findPcmData(entry, pcmBytes);
          Serial.printf("WAV: %s (%s, %lu PCM bytes)\n", childPath.c_str(),
                        supported ? "PCM" : "unsupported", static_cast<unsigned long>(pcmBytes));
        }
      }
    }
    entry.close();
  }
  directory.close();
}

static constexpr uint32_t kWatchdogConfirmMs = 5000;

static bool watchdogTestIdle() {
  return recorderState() == CaptureState::idle && playbackGetState() == PlaybackState::idle &&
         liveAudioClient() < 0 && !recorderMicSignalStatus().enabled &&
         !recorderMicSignalStatus().active && !radioProgrammingBusy() &&
         !pttTestActive();
}

static void handleSerialCommand(char command, int32_t &steps, ButtonEvent &button, bool &frameRequested) {
  static char deleteFilename[64] = {};
  static uint8_t deleteFilenameLength = 0;
  static bool receivingDeleteFilename = false;
  static bool deleteFilenameOverflow = false;
  static char radioPlaybackPath[221] = {};
  static size_t radioPlaybackLength = 0;
  static bool receivingRadioPlaybackPath = false;
  static bool radioPlaybackOverflow = false;
  static bool watchdogArmed = false;
  static uint32_t watchdogArmedAt = 0;

  if (receivingRadioPlaybackPath) {
    if (command == '\r') return;
    if (command == '\n') {
      radioPlaybackPath[radioPlaybackLength] = '\0';
      String path(radioPlaybackPath);
      String lowerPath = path;
      lowerPath.toLowerCase();
      const char *reason = nullptr;
      if (radioPlaybackOverflow || !safeSdPath(path) || path == "/" || !lowerPath.endsWith(".wav"))
        reason = "invalid WAV path";
      else if (!appSdReady() || !playbackStatus().radioReady || !appCanStartPlayback() || pttTestActive())
        reason = "SD, DAC or playback unavailable";
      else {
        File file = SD_MMC.open(path, FILE_READ);
        uint32_t remaining = 0;
        if (!file || file.isDirectory() || !findPcmData(file, remaining) || remaining == 0)
          reason = "WAV missing or unsupported";
        if (file) file.close();
      }
      if (reason) Serial.printf("PLAYBACK_TEST refused: %s\n", reason);
      else {
        Serial.printf("PLAYBACK_TEST accepted: %s\n", path.c_str());
        playbackStart(path, PlaybackTarget::radio);
      }
      receivingRadioPlaybackPath = false;
      return;
    }
    if (radioPlaybackLength < sizeof(radioPlaybackPath) - 1) radioPlaybackPath[radioPlaybackLength++] = command;
    else radioPlaybackOverflow = true;
    return;
  }

  if (receivingDeleteFilename) {
    if (command == '\r') return;
    if (command == '\n') {
      deleteFilename[deleteFilenameLength] = '\0';
      bool deleted = !deleteFilenameOverflow && recorderDeleteRecording(deleteFilename);
      Serial.printf(deleted ? "Test WAV deleted: %s\n" : "Test WAV delete refused: %s\n", deleteFilename);
      receivingDeleteFilename = false;
      return;
    }
    if (deleteFilenameLength < sizeof(deleteFilename) - 1) deleteFilename[deleteFilenameLength++] = command;
    else deleteFilenameOverflow = true;
    return;
  }

  if (command == 'x') {
    deleteFilenameLength = 0;
    deleteFilenameOverflow = false;
    receivingDeleteFilename = true;
    Serial.println("Delete test WAV: send exact recording path");
    return;
  }
  if (command == 'P') {
    radioPlaybackLength = 0;
    radioPlaybackOverflow = false;
    receivingRadioPlaybackPath = true;
    Serial.println("PLAYBACK_TEST send exact absolute WAV path and newline");
    return;
  }
  if (command == 'Z') {
    bool stopped = playbackGetState() != PlaybackState::idle &&
                   playbackStatus().target == PlaybackTarget::radio && playbackStop();
    Serial.println(stopped ? "PLAYBACK_TEST stop requested" : "PLAYBACK_TEST already idle");
    return;
  }
  if (command == 'L') {
    if (!appSdReady() || recorderState() != CaptureState::idle || playbackGetState() != PlaybackState::idle) {
      Serial.println("WAV_LIST refused: SD unavailable or device busy");
    } else {
      uint16_t remaining = 128;
      listSerialWavs("/", 0, remaining);
      Serial.printf("WAV_LIST end%s\n", remaining ? "" : " (entry limit reached)");
    }
    return;
  }
  if (command == 'w') {
    watchdogArmed = watchdogTestIdle();
    watchdogArmedAt = millis();
    Serial.println(watchdogArmed ? "Watchdog test: confirm with W within 5 s" :
                   "Watchdog test refused: device busy");
    return;
  }
  if (command == 'W') {
    bool confirmed = watchdogArmed && static_cast<uint32_t>(millis() - watchdogArmedAt) <= kWatchdogConfirmMs;
    watchdogArmed = false;
    if (!confirmed || !watchdogTestIdle()) {
      Serial.println("Watchdog test refused: not armed, expired or device busy");
      return;
    }
    Serial.println("Watchdog test: forcing a main-loop stall");
    while (true) {}
  }
  watchdogArmed = false;
  if (command == 'j') --steps;
  if (command == 'l') ++steps;
  if (command == 'k' && button == ButtonEvent::none) button = ButtonEvent::shortPress;
  if (command == 'b' && button == ButtonEvent::none) button = ButtonEvent::longPress;
  if (command == 'd') frameRequested = true;
  if (command == 'p') printDisplayState();
  if (command == 'o') displaySleep();
  if (command == 'r' && !appStartRecording()) Serial.println("Cannot start recording");
  if (command == 's') Serial.println(appStopRecording() ? "Stop requested" : "Not recording");
  if (command == 'i') printSetupStatus();
  if (command == 'q') {
    time_t rtcEpoch = 0;
    if (!appRtcReadUtc(rtcEpoch)) Serial.println("RTC_TIME_ERROR invalid or unavailable");
    else Serial.printf("RTC_TIME rtc=%llu system=%llu source=%c offline=%u\n",
                       static_cast<unsigned long long>(rtcEpoch),
                       static_cast<unsigned long long>(time(nullptr)), appClockIndicator(),
                       wifiManagerModeName() == "AP" && !wifiManagerHasCredentials() ? 1 : 0);
  }
  if (command == 'y') {
    if (wifiManagerModeName() != "AP" || wifiManagerHasCredentials() ||
        recorderState() != CaptureState::idle || playbackGetState() != PlaybackState::idle ||
        radioProgrammingBusy() || pttTestActive()) {
      Serial.println("RTC_RESTART refused: offline AP and idle device required");
    } else {
      Serial.println("RTC_RESTART accepted");
      Serial.flush();
      ESP.restart();
    }
  }
  if (command == 'm') recorderToggleSerialMeter();
  if (command == 'f') {
    bool enabled = !recorderLowpass();
    recorderSetLowpass(enabled);
    Serial.println(enabled ? "3.4 kHz low-pass enabled" : "3.4 kHz low-pass disabled");
  }
}

void setup() {
  initPttTest();
  Serial.begin(115200);
  Serial.println("RadioRecorder setup starting");
  if (!initSystemWatchdog()) {
    Serial.println("Watchdog initialization failed; restarting");
    delay(100);
    ESP.restart();
  }
  Serial.println("Watchdog: ready (10 s)");
  if (!appInitSettings()) Serial.println("Settings: NVS read failed; using defaults");
  initControls();
  bool sdReady = SD_MMC.setPins(39, 38, 40) && SD_MMC.begin("/sdcard", true, false) && SD_MMC.cardType() != CARD_NONE;
  appSetSdReady(sdReady);
  if (sdReady) recorderSetRecordingsPresent(hasRecordings());
  initRecorderAdc();
  initRecorderMic();
  initPlayback(sdReady);
  initRecorderTask();
  if (recorderTaskReady()) initLiveAudio();
  initWeb();
  if (!wifiManagerInit()) Serial.println("Wi-Fi initialization failed");
  initDisplay();
  playbackPlayCue(PlaybackCue::startup);
  delay(850);
  displayShowHome();
  printSetupStatus();
}

void loop() {
  pollPttTest();
  radioProgrammingPoll();
  int32_t steps = takeEncoderSteps();
  pollSerialMeter();
  ButtonEvent button = pollEncoderButton();
  if (steps || button != ButtonEvent::none) wifiManagerNotifyActivity();
  bool frameRequested = false;
  radioSerialPoll();
  for (uint16_t budget = 0; budget < 128 && Serial.available(); ++budget) {
    char command = Serial.read();
    if (!radioSerialConsume(command)) handleSerialCommand(command, steps, button, frameRequested);
    if (!radioSerialReceiving()) break;
  }
  pollDisplay(steps, button);
  if (frameRequested) printDisplayFrame();
  pollWeb();
  playbackPollSettings();
  wifiManagerPoll();
  appClockPoll();
  feedSystemWatchdog();
  delay(10);
}