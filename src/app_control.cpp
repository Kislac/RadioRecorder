#include "app_control.h"

#include "live_audio.h"
#include "playback.h"
#include "recorder.h"
#include "web_server.h"
#include <Preferences.h>
#include <esp_timer.h>

static constexpr char kFirmwareVersion[] = "0.1.32";
static constexpr uint32_t kFirmwareBuild = FIRMWARE_BUILD;
static constexpr uint16_t kDefaultScreenOffMinutes = 5;
static constexpr uint16_t kMaxScreenOffMinutes = 999;
static constexpr char kSettingsNamespace[] = "radio-recorder";
static constexpr char kScreenOffKey[] = "screenOffMin";
static uint16_t screenOffMinutes = kDefaultScreenOffMinutes;
static bool sdReady = false;
static uint8_t wifiTestPhase = 0;
static uint32_t wifiTestSince = 0;

void appSetSdReady(bool ready) { sdReady = ready; }
bool appSdReady() { return sdReady; }
bool appWifiTestActive() { return wifiTestPhase != 0; }

String appFirmwareVersion() {
  return String(kFirmwareVersion) + "." + String(kFirmwareBuild);
}

String appFirmwareBuildDateTime() {
  return String(__DATE__) + " " + String(__TIME__);
}

uint32_t appDemoTimeSeconds() {
  return (13UL * 3600 + 54UL * 60 + 22UL + esp_timer_get_time() / 1000000ULL) % 86400;
}

bool appInitSettings() {
  Preferences preferences;
  if (!preferences.begin(kSettingsNamespace, true)) return false;
  screenOffMinutes = preferences.getUShort(kScreenOffKey, kDefaultScreenOffMinutes);
  preferences.end();
  if (screenOffMinutes > kMaxScreenOffMinutes) screenOffMinutes = kDefaultScreenOffMinutes;
  return true;
}

uint16_t appScreenOffMinutes() { return screenOffMinutes; }

bool appSetScreenOffMinutes(uint16_t minutes) {
  if (minutes > kMaxScreenOffMinutes) return false;
  Preferences preferences;
  if (!preferences.begin(kSettingsNamespace, false)) return false;
  size_t written = preferences.putUShort(kScreenOffKey, minutes);
  preferences.end();
  if (written != sizeof(minutes)) return false;
  screenOffMinutes = minutes;
  return true;
}

bool appStartRecording() {
  return playbackGetState() == PlaybackState::idle && recorderStart(sdReady);
}

bool appStopRecording() { return stopRecording(); }

bool appCanStartPlayback() {
  return recorderState() == CaptureState::idle && playbackGetState() == PlaybackState::idle &&
         !appWifiTestActive() && liveAudioClient() < 0;
}

bool appCanConnectLiveAudio() { return playbackGetState() == PlaybackState::idle; }

bool appStartWifiTest() {
  if (liveAudioClient() >= 0 || wifiTestPhase != 0 || !appStartRecording()) return false;
  wifiTestPhase = 1;
  return true;
}

void appPollWifiTest() {
  if (wifiTestPhase == 1) {
    if (recorderState() == CaptureState::recording) {
      wifiTestSince = millis();
      wifiTestPhase = 2;
    } else if (recorderState() == CaptureState::idle) {
      wifiTestPhase = 0;
    }
  } else if (wifiTestPhase == 2) {
    if (recorderState() == CaptureState::idle) {
      wifiTestPhase = 0;
    } else if (millis() - wifiTestSince >= 5000) {
      if (!webDisableForNoiseTest()) {
        Serial.println("Wi-Fi noise test: cannot disable Wi-Fi");
        appStopRecording();
        wifiTestPhase = 4;
      } else {
        wifiTestSince = millis();
        wifiTestPhase = 3;
      }
    }
  } else if (wifiTestPhase == 3 && millis() - wifiTestSince >= 10000) {
    appStopRecording();
    wifiTestPhase = 4;
  } else if (wifiTestPhase == 4 && recorderState() == CaptureState::idle) {
    ESP.restart();
  }
}