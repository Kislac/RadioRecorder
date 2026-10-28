#include <Arduino.h>
#include "playback.h"
#include "live_audio.h"
#include "recorder.h"
#include "app_control.h"
#include "web_server.h"
#include "controls.h"
#include "display.h"
#include "watchdog.h"
#include <SD_MMC.h>
#include <WiFi.h>

void printSetupStatus() {
  Serial.printf("Firmware: v%s\n", appFirmwareVersion().c_str());
  Serial.printf("Build time: %s\n", appFirmwareBuildDateTime().c_str());
  Serial.println("Watchdog: active (10 s, both cores)");
  Serial.printf("3.4 kHz low-pass: %s (f = toggle)\n", recorderLowpass() ? "on" : "off");
  Serial.printf("OLED screen off: %u min\n", appScreenOffMinutes());
  Serial.printf("Speaker: %s\n", playbackStatus().ready ? "ready" : "unavailable");
  Serial.printf("SD card: %s\n", appSdReady() ? "ready" : "unavailable");
  Serial.printf("ADC GPIO%d: %s\n", kAudioGpio, recorderAdcReady() ? "ready" : "unavailable");
  Serial.printf("Recorder: %s\n", appSdReady() && recorderTaskReady() ? "ready" : "unavailable");
  Serial.printf("OLED: %s\n", displayReady() ? "ready" : "unavailable (check GPIO4/5, 3V3, GND and I2C address)");
  if (webReady()) {
    Serial.print("Wi-Fi AP SSID: ");
    Serial.println(WiFi.softAPSSID());
    Serial.print("Wi-Fi AP IP: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Wi-Fi AP: unavailable");
  }
  if (recorderState() != CaptureState::idle) Serial.printf("Recording: %s\n", recorderName());
}

static void handleSerialCommand(char command, int32_t &steps, ButtonEvent &button, bool &frameRequested) {
  static char deleteFilename[13] = {};
  static uint8_t deleteFilenameLength = 0;
  static bool receivingDeleteFilename = false;
  static bool deleteFilenameOverflow = false;

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
    Serial.println("Delete test WAV: send exact RECxxxxx.wav filename");
    return;
  }
  if (command == 'w') {
    Serial.println("Watchdog test: forcing a main-loop stall");
    while (true) {}
  }
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
  if (command == 'm') recorderToggleSerialMeter();
  if (command == 'f') {
    bool enabled = !recorderLowpass();
    recorderSetLowpass(enabled);
    Serial.println(enabled ? "3.4 kHz low-pass enabled" : "3.4 kHz low-pass disabled");
  }
}

void setup() {
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
  initPlayback(sdReady);
  initRecorderTask();
  if (recorderTaskReady()) initLiveAudio();
  initWeb();
  initDisplay();
  playbackPlayCue(PlaybackCue::startup);
  delay(850);
  displayShowHome();
  printSetupStatus();
}

void loop() {
  int32_t steps = takeEncoderSteps();
  pollSerialMeter();
  ButtonEvent button = pollEncoderButton();
  bool frameRequested = false;
  if (Serial.available()) handleSerialCommand(Serial.read(), steps, button, frameRequested);
  pollDisplay(steps, button);
  if (frameRequested) printDisplayFrame();
  pollWeb();
  appPollWifiTest();
  feedSystemWatchdog();
  delay(10);
}