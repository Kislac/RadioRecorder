#include "app_control.h"

#include "live_audio.h"
#include "playback.h"
#include "recorder.h"
#include "radio_programming.h"
#include "web_server.h"
#include <Preferences.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>

static constexpr char kFirmwareVersion[] = "0.1.55";
static constexpr uint32_t kFirmwareBuild = FIRMWARE_BUILD;
static constexpr uint16_t kDefaultScreenOffMinutes = 5;
static constexpr uint16_t kMaxScreenOffMinutes = 999;
static constexpr char kSettingsNamespace[] = "radio-recorder";
static constexpr char kScreenOffKey[] = "screenOffMin";
static constexpr char kEpochKey[] = "clockEpoch";
static constexpr char kZoneKey[] = "clockZone";
static constexpr time_t kMinEpoch = 946684800;
static constexpr uint8_t kRtcAddress = 0x68;
static uint16_t screenOffMinutes = kDefaultScreenOffMinutes;
static uint8_t clockZone = 0;
static bool clockKnown = false;
static bool clockOnline = false;
static bool clockSynced = false;
static bool clockManual = false;
static bool clockDefault = true;
static bool clockFromRtc = false;
static bool rtcAvailable = false;
static bool rtcValid = false;
static int32_t savedDay = -1;
static uint32_t lastDayCheck = 0;
static uint32_t lastSyncAt = 0;
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
  if (!clockKnown) return 0;
  time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  return local.tm_hour * 3600UL + local.tm_min * 60UL + local.tm_sec;
}

static String clockTimezoneRule() {
  if (clockZone == 0) return "CET-1CEST,M3.5.0/2,M10.5.0/3";
  if (clockZone == 1) return "UTC0";
  return "UTC" + String(14 - static_cast<int>(clockZone) >= 0 ? "+" : "") +
         String(14 - static_cast<int>(clockZone));
}

static void applyClockTimezone() {
  String rule = clockTimezoneRule();
  setenv("TZ", rule.c_str(), 1);
  tzset();
}

static int32_t clockDay(time_t instant) {
  tm local;
  localtime_r(&instant, &local);
  return local.tm_year * 366 + local.tm_yday;
}

static bool saveClock(time_t instant) {
  Preferences preferences;
  if (!preferences.begin(kSettingsNamespace, false)) return false;
  bool saved = preferences.putULong64(kEpochKey, static_cast<uint64_t>(instant)) == sizeof(uint64_t);
  preferences.end();
  if (saved) savedDay = clockDay(instant);
  return saved;
}

static uint8_t rtcBcd(uint8_t value) { return (value / 10) << 4 | (value % 10); }
static uint8_t rtcDecimal(uint8_t value) { return (value >> 4) * 10 + (value & 0x0f); }
static bool rtcLeapYear(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

static bool rtcReadRegisters(uint8_t reg, uint8_t *values, uint8_t count) {
  Wire.beginTransmission(kRtcAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kRtcAddress, count) != count) return false;
  for (uint8_t index = 0; index < count; ++index) values[index] = Wire.read();
  return true;
}

static bool rtcReadUtc(time_t &instant) {
  uint8_t status = 0;
  uint8_t values[7] = {};
  if (!rtcReadRegisters(0x0f, &status, 1) || !rtcReadRegisters(0x00, values, 7) || (status & 0x80)) return false;
  if ((values[0] & 0x80) || (values[1] & 0x80) || (values[2] & 0x40) || (values[5] & 0x80)) return false;
  for (uint8_t index : {0, 1, 2, 4, 5, 6}) {
    if ((values[index] & 0x0f) > 9) return false;
  }
  tm utc = {};
  utc.tm_sec = rtcDecimal(values[0] & 0x7f);
  utc.tm_min = rtcDecimal(values[1] & 0x7f);
  utc.tm_hour = rtcDecimal(values[2] & 0x3f);
  utc.tm_mday = rtcDecimal(values[4] & 0x3f);
  utc.tm_mon = rtcDecimal(values[5] & 0x1f) - 1;
  utc.tm_year = 100 + rtcDecimal(values[6]);
  if (utc.tm_year > 199 || utc.tm_mon < 0 || utc.tm_mon > 11 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
      utc.tm_hour > 23 || utc.tm_min > 59 || utc.tm_sec > 59) return false;
  tm expected = utc;
  static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  instant = kMinEpoch;
  for (int year = 2000; year < utc.tm_year + 1900; ++year) instant += (rtcLeapYear(year) ? 366 : 365) * 86400LL;
  for (int month = 0; month < utc.tm_mon; ++month)
    instant += (monthDays[month] + (month == 1 && rtcLeapYear(utc.tm_year + 1900))) * 86400LL;
  instant += (utc.tm_mday - 1) * 86400LL + utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
  tm roundtrip = {};
  if (instant < kMinEpoch || !gmtime_r(&instant, &roundtrip) || roundtrip.tm_year != expected.tm_year ||
      roundtrip.tm_mon != expected.tm_mon || roundtrip.tm_mday != expected.tm_mday) return false;
  return true;
}

static bool rtcWriteUtc(time_t instant) {
  if (!rtcAvailable || instant < kMinEpoch || instant > 4102444799LL) return false;
  tm utc = {};
  if (!gmtime_r(&instant, &utc)) return false;
  Wire.beginTransmission(kRtcAddress);
  Wire.write(0x00);
  Wire.write(rtcBcd(utc.tm_sec));
  Wire.write(rtcBcd(utc.tm_min));
  Wire.write(rtcBcd(utc.tm_hour));
  Wire.write(rtcBcd(utc.tm_wday + 1));
  Wire.write(rtcBcd(utc.tm_mday));
  Wire.write(rtcBcd(utc.tm_mon + 1));
  Wire.write(rtcBcd(utc.tm_year - 100));
  if (Wire.endTransmission() != 0) { rtcValid = false; return false; }
  uint8_t status = 0;
  if (!rtcReadRegisters(0x0f, &status, 1)) { rtcValid = false; return false; }
  Wire.beginTransmission(kRtcAddress);
  Wire.write(0x0f);
  Wire.write(status & ~0x80);
  rtcValid = Wire.endTransmission() == 0;
  return rtcValid;
}

const char *appRtcStatus() { return !rtcAvailable ? "unavailable" : rtcValid ? "ready" : "time invalid"; }
bool appRtcReadUtc(time_t &instant) {
  rtcValid = rtcAvailable && rtcReadUtc(instant);
  return rtcValid;
}

bool appClockKnown() { return clockKnown; }
bool appClockLocal(tm &local) {
  if (!clockKnown) return false;
  time_t now = time(nullptr);
  return localtime_r(&now, &local) != nullptr;
}
static bool clockFreshSync() { return clockOnline && clockSynced && millis() - lastSyncAt < 86400000UL; }
char appClockIndicator() { return !clockKnown ? '-' : clockFreshSync() ? 'N' : clockManual ? 'K' : clockFromRtc ? 'R' : clockDefault ? 'A' : 'M'; }
uint8_t appClockTimezone() { return clockZone; }

String appClockJson() {
  time_t now = time(nullptr);
  tm local = {};
  if (clockKnown) localtime_r(&now, &local);
  char text[20] = {};
  if (clockKnown) strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
  String json = "{\"local\":\"" + String(text) + "\",\"epoch\":" +
                String(clockKnown ? static_cast<uint64_t>(now) : 0);
  json += ",\"zone\":" + String(clockZone);
  json += ",\"source\":\"" + String(!clockKnown ? "unset" : clockFreshSync() ? "internet" :
                                           clockManual ? "manual" : clockFromRtc ? "rtc" : clockDefault ? "default" : "saved") + "\"}";
  return json;
}

bool appClockSetTimezone(uint8_t zone) {
  if (zone > 28) return false;
  if (clockZone == zone) return true;
  Preferences preferences;
  if (!preferences.begin(kSettingsNamespace, false)) return false;
  bool saved = preferences.putUChar(kZoneKey, zone) == sizeof(uint8_t);
  preferences.end();
  if (!saved) return false;
  clockZone = zone;
  applyClockTimezone();
  if (clockKnown) savedDay = clockDay(time(nullptr));
  return true;
}

bool appClockSetLocal(const String &text) {
  if (text.length() != 16 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') return false;
  for (size_t index = 0; index < text.length(); ++index) {
    if (index == 4 || index == 7 || index == 10 || index == 13) continue;
    if (!isDigit(text[index])) return false;
  }
  tm requested = {};
  requested.tm_year = text.substring(0, 4).toInt() - 1900;
  requested.tm_mon = text.substring(5, 7).toInt() - 1;
  requested.tm_mday = text.substring(8, 10).toInt();
  requested.tm_hour = text.substring(11, 13).toInt();
  requested.tm_min = text.substring(14, 16).toInt();
  requested.tm_isdst = -1;
  if (requested.tm_year < 120 || requested.tm_year > 199 || requested.tm_mon < 0 || requested.tm_mon > 11 ||
      requested.tm_mday < 1 || requested.tm_mday > 31 || requested.tm_hour > 23 || requested.tm_min > 59) return false;
  time_t instant = mktime(&requested);
  char roundtrip[17];
  if (instant < kMinEpoch || strftime(roundtrip, sizeof(roundtrip), "%Y-%m-%dT%H:%M", &requested) == 0 ||
      text != roundtrip) return false;
  if (!saveClock(instant)) return false;
  timeval tv = {instant, 0};
  if (settimeofday(&tv, nullptr) != 0) return false;
  clockKnown = true;
  clockSynced = false;
  clockManual = true;
  clockDefault = false;
  clockFromRtc = false;
  rtcWriteUtc(instant);
  return true;
}

void appClockStartSync() {
  clockOnline = true;
  clockSynced = false;
  String rule = clockTimezoneRule();
  configTzTime(rule.c_str(), "pool.ntp.org", "time.cloudflare.com");
}

void appClockStopSync() { clockOnline = false; clockSynced = false; }

void appClockPoll() {
  if (clockOnline && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && time(nullptr) >= kMinEpoch) {
    time_t now = time(nullptr);
    if (!clockSynced || !clockKnown || clockDay(now) != savedDay) saveClock(now);
    rtcWriteUtc(now);
    clockKnown = true;
    clockSynced = true;
    clockManual = false;
    clockDefault = false;
    clockFromRtc = false;
    lastSyncAt = millis();
  }
  if (!clockKnown || millis() - lastDayCheck < 60000UL) return;
  lastDayCheck = millis();
  time_t now = time(nullptr);
  if (clockDay(now) != savedDay) saveClock(now);
}

bool appInitSettings() {
  timeval fallback = {kMinEpoch, 0};
  clockKnown = settimeofday(&fallback, nullptr) == 0;
  if (clockKnown) savedDay = clockDay(kMinEpoch);
  if (Wire.begin(4, 5)) {
    Wire.beginTransmission(kRtcAddress);
    rtcAvailable = Wire.endTransmission() == 0;
  }
  time_t rtcEpoch = 0;
  rtcValid = rtcAvailable && rtcReadUtc(rtcEpoch);
  Preferences preferences;
  bool nvsReady = preferences.begin(kSettingsNamespace, true);
  uint64_t savedEpoch = 0;
  if (nvsReady) {
    screenOffMinutes = preferences.getUShort(kScreenOffKey, kDefaultScreenOffMinutes);
    clockZone = preferences.getUChar(kZoneKey, 0);
    savedEpoch = preferences.getULong64(kEpochKey, 0);
    preferences.end();
  }
  if (screenOffMinutes > kMaxScreenOffMinutes) screenOffMinutes = kDefaultScreenOffMinutes;
  if (clockZone > 28) clockZone = 0;
  applyClockTimezone();
  bool savedValid = savedEpoch >= static_cast<uint64_t>(kMinEpoch) && savedEpoch <= 4102444799ULL;
  if (rtcValid || savedValid) {
    bool useRtc = rtcValid && (!savedValid || rtcEpoch > static_cast<time_t>(savedEpoch));
    timeval tv = {useRtc ? rtcEpoch : static_cast<time_t>(savedEpoch), 0};
    clockKnown = settimeofday(&tv, nullptr) == 0;
    if (clockKnown) { savedDay = clockDay(tv.tv_sec); clockDefault = false; clockFromRtc = useRtc; }
  }
  return nvsReady;
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

bool appStartMicRecording() {
  return playbackGetState() == PlaybackState::idle && liveAudioClient() < 0 && !appWifiTestActive() &&
         recorderStart(sdReady, CaptureSource::microphone);
}

bool appStopRecording() { return stopRecording(); }

bool appCanStartPlayback() {
  return recorderState() == CaptureState::idle && playbackGetState() == PlaybackState::idle &&
         !appWifiTestActive() && liveAudioClient() < 0 &&
         !recorderMicSignalStatus().enabled && !recorderMicSignalStatus().active;
}

bool appCanConnectLiveAudio() { return playbackGetState() == PlaybackState::idle; }

bool appStartWifiTest() {
  if (radioProgrammingBusy()) return false;
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