#include <Arduino.h>
#include "playback.h"
#include "live_audio.h"
#include "recorder.h"
#include "app_control.h"
#include "web_server.h"
#include "controls.h"
#include "display.h"
#include <SD_MMC.h>
#include <WiFi.h>

void printSetupStatus() {
  Serial.printf("Firmware: v%s\n", appFirmwareVersion().c_str());
  Serial.printf("3.4 kHz low-pass: %s (f = toggle)\n", recorderLowpass() ? "on" : "off");
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

void setup() {
  Serial.begin(115200);
  Serial.println("RadioRecorder setup starting");
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
  printSetupStatus();
}

void loop() {
  int32_t steps = takeEncoderSteps();
  pollSerialMeter();
  ButtonEvent button = pollEncoderButton();
  pollDisplay(steps, button);
  pollWeb();
  appPollWifiTest();
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' && !appStartRecording()) Serial.println("Cannot start recording");
    if (command == 's') Serial.println(appStopRecording() ? "Stop requested" : "Not recording");
    if (command == 'i') printSetupStatus();
    if (command == 'f') {
      bool enabled = !recorderLowpass();
      recorderSetLowpass(enabled);
      Serial.println(enabled ? "3.4 kHz low-pass enabled" : "3.4 kHz low-pass disabled");
    }
  }
  delay(10);
}