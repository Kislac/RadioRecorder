#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include "app_control.h"
#include "live_audio.h"
#include "playback.h"
#include "recorder.h"
#include "web_server.h"

namespace {
constexpr char kApSsid[] = "RadioRecorder";
constexpr char kNamespace[] = "radio-recorder";
constexpr char kSsidKey[] = "wifiSsid";
constexpr char kPasswordKey[] = "wifiPassword";
constexpr char kKeepApKey[] = "keepApHost";
constexpr char kWifiTimeoutKey[] = "wifiOffMin";
constexpr uint16_t kDefaultTimeoutMinutes = 5;
constexpr uint16_t kMaxTimeoutMinutes = 999;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint8_t kMaxScanResults = 20;

enum class Mode : uint8_t { off, ap, connecting, station, apStation };
Mode mode = Mode::off;
String savedSsid;
String savedPassword;
String pendingSsid;
String pendingPassword;
bool keepAp = false;
bool pendingKeepAp = false;
bool connectingWithNewCredentials = false;
uint16_t autoOffMinutes = kDefaultTimeoutMinutes;
uint32_t connectStartedAt = 0;
uint32_t lastActivityAt = 0;
bool scanRunning = false;
bool scanComplete = false;
bool scanFailed = false;
bool restoreApOnlyAfterScan = false;
bool forgetPending = false;
bool clockStationConnected = false;
uint8_t scanResultCount = 0;
struct ScanResult {
  String ssid;
  int32_t rssi;
  bool open;
};
ScanResult scanResults[kMaxScanResults];

bool saveNetwork(const String &ssid, const String &password, bool keepApHost) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  size_t ssidBytes = preferences.putString(kSsidKey, ssid);
  size_t passwordBytes = preferences.putString(kPasswordKey, password);
  size_t keepApBytes = preferences.putBool(kKeepApKey, keepApHost);
  preferences.end();
  return ssidBytes == ssid.length() && passwordBytes == password.length() && keepApBytes == sizeof(uint8_t);
}

bool startAp(bool withStation) {
  wifi_mode_t wifiMode = withStation ? WIFI_AP_STA : WIFI_AP;
  if (!WiFi.mode(wifiMode) || !WiFi.softAP(kApSsid)) return false;
  mode = withStation ? Mode::apStation : Mode::ap;
  webStartServices();
  Serial.print("Wi-Fi AP: http://");
  Serial.println(WiFi.softAPIP());
  return true;
}

void fallbackToAp(const char *reason) {
  Serial.printf("Wi-Fi station fallback: %s\n", reason);
  appClockStopSync();
  clockStationConnected = false;
  WiFi.disconnect(false, false);
  mode = Mode::off;
  if (!startAp(false)) {
    WiFi.mode(WIFI_OFF);
    webStopServices();
    mode = Mode::off;
    Serial.println("Wi-Fi AP start failed");
  }
  lastActivityAt = millis();
}

void beginSavedConnection() {
  if (savedSsid.isEmpty()) {
    mode = Mode::off;
    startAp(false);
    return;
  }
  if (!startAp(true)) {
    fallbackToAp("cannot start AP+STA");
    return;
  }
  pendingSsid = savedSsid;
  pendingPassword = savedPassword;
  pendingKeepAp = keepAp;
  connectingWithNewCredentials = false;
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  connectStartedAt = millis();
  mode = Mode::connecting;
  Serial.printf("Wi-Fi: connecting to saved SSID '%s'\n", pendingSsid.c_str());
}

bool hasApInterface() { return mode == Mode::ap || mode == Mode::apStation || mode == Mode::connecting; }
}

bool wifiManagerInit() {
  Preferences preferences;
  if (preferences.begin(kNamespace, false)) {
    savedSsid = preferences.getString(kSsidKey, "");
    savedPassword = preferences.getString(kPasswordKey, "");
    keepAp = preferences.getBool(kKeepApKey, false);
    autoOffMinutes = preferences.getUShort(kWifiTimeoutKey, kDefaultTimeoutMinutes);
    preferences.end();
  } else {
    Serial.println("Wi-Fi settings NVS unavailable; starting open AP with defaults");
    savedSsid = "";
    savedPassword = "";
    keepAp = false;
    autoOffMinutes = kDefaultTimeoutMinutes;
  }
  if (autoOffMinutes > kMaxTimeoutMinutes) autoOffMinutes = kDefaultTimeoutMinutes;
  lastActivityAt = millis();
  beginSavedConnection();
  return mode != Mode::off;
}

void wifiManagerPoll() {
  if (appWifiTestActive()) return;
  uint32_t now = millis();

  if (forgetPending) {
    forgetPending = false;
    appClockStopSync();
    clockStationConnected = false;
    webStopServices();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    mode = Mode::off;
    if (!startAp(false)) Serial.println("Wi-Fi AP start failed after forgetting network");
    lastActivityAt = millis();
    return;
  }

  if (mode == Mode::connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      String connectedSsid = WiFi.SSID();
      if (connectingWithNewCredentials && !saveNetwork(pendingSsid, pendingPassword, pendingKeepAp)) {
        connectingWithNewCredentials = false;
        fallbackToAp("could not save Wi-Fi credentials to NVS");
        pendingSsid = "";
        pendingPassword = "";
        return;
      }
      if (connectingWithNewCredentials) {
        savedSsid = pendingSsid;
        savedPassword = pendingPassword;
        keepAp = pendingKeepAp;
      }
      if (keepAp) {
        mode = Mode::apStation;
      } else {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        mode = Mode::station;
      }
      Serial.printf("Wi-Fi connected: '%s', IP %s, mode %s\n", connectedSsid.c_str(),
                    WiFi.localIP().toString().c_str(), wifiManagerModeName().c_str());
      appClockStartSync();
      clockStationConnected = true;
      connectingWithNewCredentials = false;
      pendingSsid = "";
      pendingPassword = "";
      lastActivityAt = now;
    } else if (now - connectStartedAt >= kConnectTimeoutMs) {
      connectingWithNewCredentials = false;
      fallbackToAp("connection timed out after 15 seconds");
      pendingSsid = "";
      pendingPassword = "";
    }
  } else if (mode == Mode::station && WiFi.status() != WL_CONNECTED) {
    fallbackToAp("station connection lost");
  }
  if (mode == Mode::apStation) {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !clockStationConnected) appClockStartSync();
    if (!connected && clockStationConnected) appClockStopSync();
    clockStationConnected = connected;
  }

  if (scanRunning) {
    int result = WiFi.scanComplete();
    if (result >= 0) {
      scanRunning = false;
      scanComplete = true;
      scanFailed = false;
      scanResultCount = static_cast<uint8_t>(result < kMaxScanResults ? result : kMaxScanResults);
      for (uint8_t index = 0; index < scanResultCount; ++index) {
        scanResults[index].ssid = WiFi.SSID(index);
        scanResults[index].rssi = WiFi.RSSI(index);
        scanResults[index].open = WiFi.encryptionType(index) == WIFI_AUTH_OPEN;
      }
      WiFi.scanDelete();
      if (restoreApOnlyAfterScan && mode == Mode::apStation) {
        WiFi.mode(WIFI_AP);
        mode = Mode::ap;
      }
    } else if (result == WIFI_SCAN_FAILED) {
      scanRunning = false;
      scanComplete = false;
      scanFailed = true;
      WiFi.scanDelete();
      if (restoreApOnlyAfterScan && mode == Mode::apStation) {
        WiFi.mode(WIFI_AP);
        mode = Mode::ap;
      }
    }
  }

  bool busy = recorderState() != CaptureState::idle || liveAudioClient() >= 0 ||
              playbackGetState() != PlaybackState::idle || mode == Mode::connecting || scanRunning;
  if (mode != Mode::off && !busy && autoOffMinutes > 0 && now - lastActivityAt >= static_cast<uint32_t>(autoOffMinutes) * 60000UL) {
    appClockStopSync();
    clockStationConnected = false;
    webStopServices();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    mode = Mode::off;
    Serial.println("Wi-Fi auto-off: inactivity timeout");
  }
}

void wifiManagerNotifyActivity() {
  lastActivityAt = millis();
}

void wifiManagerWake() {
  lastActivityAt = millis();
  if (mode == Mode::off) beginSavedConnection();
}

bool wifiManagerToggle() {
  if (appWifiTestActive() || mode == Mode::connecting) return false;
  if (mode == Mode::off) {
    wifiManagerWake();
    return mode != Mode::off;
  }
  webStopServices();
  appClockStopSync();
  clockStationConnected = false;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  mode = Mode::off;
  return true;
}

bool wifiManagerRequestScan() {
  if (mode == Mode::connecting) return false;
  wifiManagerNotifyActivity();
  scanRunning = false;
  scanComplete = false;
  scanFailed = false;
  scanResultCount = 0;
  WiFi.scanDelete();
  restoreApOnlyAfterScan = mode == Mode::ap;
  if (restoreApOnlyAfterScan) {
    if (!WiFi.mode(WIFI_AP_STA)) return false;
    mode = Mode::apStation;
  }
  int result = WiFi.scanNetworks(true, false);
  if (result == WIFI_SCAN_FAILED) {
    scanFailed = true;
    scanResultCount = 0;
    if (restoreApOnlyAfterScan) {
      WiFi.mode(WIFI_AP);
      mode = Mode::ap;
    }
    return false;
  }
  scanRunning = result == WIFI_SCAN_RUNNING;
  if (result >= 0) {
    scanComplete = true;
    scanResultCount = static_cast<uint8_t>(result < kMaxScanResults ? result : kMaxScanResults);
    for (uint8_t index = 0; index < scanResultCount; ++index) {
      scanResults[index].ssid = WiFi.SSID(index);
      scanResults[index].rssi = WiFi.RSSI(index);
      scanResults[index].open = WiFi.encryptionType(index) == WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    if (restoreApOnlyAfterScan) {
      WiFi.mode(WIFI_AP);
      mode = Mode::ap;
    }
  }
  return true;
}

String wifiManagerScanJson() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddStringToObject(root, "state", scanRunning ? "scanning" : scanFailed ? "failed" : scanComplete ? "complete" : "idle");
  cJSON *networks = cJSON_AddArrayToObject(root, "networks");
  if (networks && scanComplete) {
    for (uint8_t index = 0; index < scanResultCount; ++index) {
      cJSON *network = cJSON_CreateObject();
      if (!network) continue;
      cJSON_AddStringToObject(network, "ssid", scanResults[index].ssid.c_str());
      cJSON_AddNumberToObject(network, "rssi", scanResults[index].rssi);
      cJSON_AddBoolToObject(network, "open", scanResults[index].open);
      cJSON_AddItemToArray(networks, network);
    }
    scanComplete = false;
    scanResultCount = 0;
  }
  char *json = cJSON_PrintUnformatted(root);
  String result = json ? String(json) : String("{}");
  if (json) cJSON_free(json);
  cJSON_Delete(root);
  return result;
}

bool wifiManagerConnect(const String &ssid, const String &password, bool keepApHost) {
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64 || recorderState() != CaptureState::idle ||
      appWifiTestActive() || mode == Mode::connecting) return false;
  for (char character : ssid) if (static_cast<uint8_t>(character) < 32) return false;
  if (mode == Mode::off && !startAp(false)) return false;
  bool apAlreadyActive = hasApInterface();
  if (!WiFi.mode(WIFI_AP_STA) || (!apAlreadyActive && !WiFi.softAP(kApSsid))) return false;
  webStartServices();
  WiFi.scanDelete();
  appClockStopSync();
  clockStationConnected = false;
  scanRunning = scanComplete = scanFailed = false;
  WiFi.disconnect(false, false);
  pendingSsid = ssid;
  pendingPassword = password;
  pendingKeepAp = keepApHost;
  connectingWithNewCredentials = true;
  mode = Mode::connecting;
  connectStartedAt = millis();
  lastActivityAt = connectStartedAt;
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
  return true;
}

bool wifiManagerForget() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  bool ssidRemoved = !preferences.isKey(kSsidKey) || preferences.remove(kSsidKey);
  bool passwordRemoved = !preferences.isKey(kPasswordKey) || preferences.remove(kPasswordKey);
  bool keepApCleared = preferences.putBool(kKeepApKey, false) == sizeof(uint8_t);
  preferences.end();
  if (!ssidRemoved || !passwordRemoved || !keepApCleared) return false;
  savedSsid = "";
  savedPassword = "";
  keepAp = false;
  pendingSsid = "";
  pendingPassword = "";
  pendingKeepAp = false;
  connectingWithNewCredentials = false;
  WiFi.scanDelete();
  scanRunning = scanComplete = scanFailed = false;
  if (mode == Mode::off || mode == Mode::station || mode == Mode::apStation || mode == Mode::connecting) {
    forgetPending = true;
    lastActivityAt = millis();
    return true;
  }
  lastActivityAt = millis();
  return true;
}

String wifiManagerStatusJson() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddStringToObject(root, "mode", wifiManagerModeName().c_str());
  cJSON_AddStringToObject(root, "ssid", wifiManagerSSID().c_str());
  cJSON_AddStringToObject(root, "ip", wifiManagerIPAddress().c_str());
  cJSON_AddStringToObject(root, "savedSsid", savedSsid.c_str());
  cJSON_AddBoolToObject(root, "hasSavedNetwork", !savedSsid.isEmpty());
  cJSON_AddBoolToObject(root, "keepApHost", keepAp);
  cJSON_AddBoolToObject(root, "connecting", mode == Mode::connecting);
  cJSON_AddBoolToObject(root, "apAvailable", hasApInterface());
  cJSON_AddNumberToObject(root, "autoOffMinutes", autoOffMinutes);
  char *json = cJSON_PrintUnformatted(root);
  String result = json ? String(json) : String("{}");
  if (json) cJSON_free(json);
  cJSON_Delete(root);
  return result;
}

String wifiManagerModeName() {
  switch (mode) {
    case Mode::off: return "OFF";
    case Mode::ap: return "AP";
    case Mode::connecting: return "CONNECTING";
    case Mode::station: return "STA";
    case Mode::apStation: return "AP+STA";
  }
  return "UNKNOWN";
}

String wifiManagerIPAddress() {
  if (mode == Mode::station || mode == Mode::apStation || (mode == Mode::connecting && WiFi.status() == WL_CONNECTED)) {
    return WiFi.localIP().toString();
  }
  if (hasApInterface()) return WiFi.softAPIP().toString();
  return "-";
}

String wifiManagerSSID() {
  if (mode == Mode::connecting) return pendingSsid;
  if (mode == Mode::station || mode == Mode::apStation) return WiFi.SSID();
  return hasApInterface() ? String(kApSsid) : String("-");
}

String wifiManagerSavedSSID() { return savedSsid; }
bool wifiManagerKeepApHost() { return keepAp; }
bool wifiManagerHasCredentials() { return !savedSsid.isEmpty(); }
uint16_t wifiManagerAutoOffMinutes() { return autoOffMinutes; }

bool wifiManagerSetAutoOffMinutes(uint16_t minutes) {
  if (minutes > kMaxTimeoutMinutes) return false;
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  size_t written = preferences.putUShort(kWifiTimeoutKey, minutes);
  preferences.end();
  if (written != sizeof(minutes)) return false;
  autoOffMinutes = minutes;
  lastActivityAt = millis();
  return true;
}