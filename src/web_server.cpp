#include <Arduino.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <cJSON.h>
#include <functional>
#include "web_server.h"
#include "web_pages.h"
#include "wav.h"
#include "playback.h"
#include "recorder.h"
#include "live_audio.h"
#include "app_control.h"
#include "language.h"
#include "file_manager.h"
#include "automatic_record.h"
#include "automatic_replay.h"
#include "display.h"
#include "wifi_manager.h"
#include "ptt_test.h"
#include "watchdog.h"
#include "radio_programming.h"
#include "radio_patch.h"
#include "performance.h"

static WebServer server(80);
static WebSocketsServer audioSocket(81);
static bool wifiReady = false;
static bool routesReady = false;

bool webReady() { return wifiReady; }

static void onAudioSocket(uint8_t client, WStype_t event, uint8_t *, size_t) {
  wifiManagerNotifyActivity();
  if (event == WStype_CONNECTED) {
    if (!recorderAdcReady() || !recorderTaskReady() || !liveAudioReady() || liveAudioClient() >= 0 ||
      !appCanConnectLiveAudio() || !connectLiveAudio(client)) {
      audioSocket.disconnect(client);
      return;
    }
    recorderNotify();
    Serial.println("Live audio connected");
  } else if (event == WStype_DISCONNECTED && disconnectLiveAudio(client)) {
    Serial.println("Live audio disconnected");
  }
}

static void sendLiveFrames() {
  int client = liveAudioClient();
  if (client < 0 || !liveAudioReady()) return;
  if (takeLiveAudioFault()) {
    audioSocket.disconnect(client);
    return;
  }
  AudioFrame frame;
  if (popLiveAudio(frame) && !audioSocket.sendBIN(client, frame.samples, frame.length)) {
    audioSocket.disconnect(client);
  }
}


bool isRecordingFile(const String &name) {
  String upper = name;
  upper.toUpperCase();
  if (!upper.endsWith(".WAV")) return false;
  if (upper.length() == 12 && upper.startsWith("REC")) {
    for (int index = 3; index < 8; ++index) if (!isDigit(name[index])) return false;
    return true;
  }
  size_t prefix = 0;
  if (upper.startsWith("RADIOMAN") || upper.startsWith("RADIOAUT")) prefix = 8;
  else if (upper.startsWith("MICMAN") || upper.startsWith("MICAUT")) prefix = 6;
  if (!prefix || (name.length() != prefix + 17 && name.length() != prefix + 20)) return false;
  for (int index : {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12})
    if (!isDigit(name[prefix + index])) return false;
  if (name[prefix + 6] != '_') return false;
  return name.length() == prefix + 17 ||
         (name[prefix + 13] == '_' && isDigit(name[prefix + 14]) && isDigit(name[prefix + 15]));
}

bool isDatedFolder(const String &name) {
  if (name.length() != 10 || name[4] != '-' || name[7] != '-') return false;
  for (int index : {0, 1, 2, 3, 5, 6, 8, 9}) if (!isDigit(name[index])) return false;
  return true;
}

String fileName(const File &file) {
  String name = file.name();
  return name.substring(name.lastIndexOf('/') + 1);
}

bool visitRecordingFolder(const String &folder, const std::function<bool(const String &, size_t)> &visit,
                          uint8_t depth, bool &continuing) {
  if (depth > 16) return false;
  uint32_t position = 0, lastYield = millis();
  while (continuing) {
    File directory = SD_MMC.open(folder, FILE_READ), entry;
    if (!directory) return false;
    for (uint32_t index = 0; index < position; ++index) { entry = directory.openNextFile(); entry.close(); }
    entry = directory.openNextFile();
    if (!entry) { directory.close(); break; }
    ++position;
    String name = fileName(entry), path = folder == "/" ? "/" + name : folder + "/" + name;
    bool nested = entry.isDirectory(); size_t size = entry.size();
    entry.close(); directory.close();
    if (name.startsWith(".rr-") || !managedPathValid(path.c_str(), false)) continue;
    if (nested) { if (!visitRecordingFolder(path, visit, depth + 1, continuing)) return false; }
    else { name.toLowerCase(); if (name.endsWith(".wav") && size >= 44) continuing = visit(path, size); }
    watchdogResetCurrentTask();
    if (millis() - lastYield >= 5) { delay(1); lastYield = millis(); }
  }
  return true;
}

bool visitRecordings(const std::function<bool(const String &, size_t)> &visit) {
  bool continuing = true;
  return visitRecordingFolder("/", visit, 0, continuing);
}

bool hasRecordings() {
  bool found = false;
  visitRecordings([&found](const String &, size_t) { found = true; return false; });
  return found;
}

void sendStatus() {
  wifiManagerNotifyActivity();
  CaptureState state = recorderState();
  bool ready = appSdReady() && recorderAdcReady() && recorderTaskReady();
  String json = "{\"ready\":" + String(ready ? "true" : "false");
  json += ",\"version\":\"" + appFirmwareVersion() + "\"";
  json += ",\"fileBusy\":" + String(fileManagerBusy() ? "true" : "false");
  json += ",\"screenOffMinutes\":" + String(appScreenOffMinutes());
  json += ",\"wifi\":" + wifiManagerStatusJson();
  json += ",\"demoSeconds\":" + String(appDemoTimeSeconds());
  json += ",\"clock\":" + appClockJson();
  json += ",\"recordElapsedMs\":" + String(recorderElapsedMs());
  json += ",\"recordingMode\":\"" + String(recordingModeCode(automaticCurrentMode())) + "\"";
  json += ",\"automatic\":" + automaticRecordingJson();
  json += ",\"audioGpio\":" + String(kAudioGpio);
  json += ",\"lowpass\":" + String(recorderLowpass() ? "true" : "false");
  json += ",\"meterReady\":" + String(recorderAdcReady() && recorderTaskReady() ? "true" : "false");
  json += ",\"micReady\":" + String(appSdReady() && recorderMicReady() && recorderTaskReady() ? "true" : "false");
  json += ",\"recordSource\":\"" + String(recorderSource() == CaptureSource::microphone ? "microphone" : "adc") + "\"";
  json += ",\"liveReady\":" + String(recorderAdcReady() && recorderTaskReady() && liveAudioReady() && wifiReady ? "true" : "false");
  json += ",\"state\":\"" + String(state == CaptureState::idle ? "idle" : state == CaptureState::starting ? "starting" : "recording") + "\"";
  PlaybackStatus speaker = playbackStatus();
  PlaybackState playback = speaker.state;
  json += ",\"speakerReady\":" + String(speaker.ready ? "true" : "false");
  json += ",\"radioReady\":" + String(speaker.radioReady ? "true" : "false");
  json += ",\"speakerVolume\":" + String(speaker.volume);
  json += ",\"speakerBoost\":" + String(speaker.boost);
  json += ",\"radioVolume\":" + String(speaker.radioVolume);
  json += ",\"radioBoost\":" + String(speaker.radioBoost);
  json += ",\"playbackBytesPlayed\":" + String(speaker.bytesPlayed);
  json += ",\"playbackBytesTotal\":" + String(speaker.bytesTotal);
  json += ",\"playbackPaused\":" + String(speaker.paused ? "true" : "false");
  json += ",\"radioPttActive\":" + String(playbackPttActive() ? "true" : "false");
  json += ",\"playback\":\"" + String(playback == PlaybackState::idle ? "idle" : playback == PlaybackState::starting ? "starting" : "playing") + "\"";
  json += ",\"playbackTarget\":\"" + String(speaker.target == PlaybackTarget::radio ? "radio" : "speaker") + "\"";
  json += ",\"playbackPath\":\"" + speaker.path + "\"";
  json += ",\"playbackError\":\"" + speaker.error + "\"";
  json += ",\"stopping\":" + String(recorderStopping() ? "true" : "false");
  json += ",\"hasRecordings\":" + String(ready && state == CaptureState::idle && recorderHasRecordings() ? "true" : "false");
  json += ",\"error\":\"" + String(state == CaptureState::idle ? recorderError() : "") + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void sendSettings() {
  wifiManagerNotifyActivity();
  server.sendHeader("Cache-Control", "no-store");
  String json = "{\"screenOffMinutes\":" + String(appScreenOffMinutes());
  json += ",\"language\":\"" + String(appLanguage()) + "\"";
  json += ",\"radioTotSeconds\":" + String(playbackRadioTotSeconds());
  json += ",\"performanceEnabled\":" + String(performanceEnabled() ? "true" : "false");
  json += ",\"wifiAutoOffMinutes\":" + String(wifiManagerAutoOffMinutes());
  json += ",\"wifi\":" + wifiManagerStatusJson() + "}";
  server.send(200, "application/json", json);
}

void sendRecordCues() {
  wifiManagerNotifyActivity();
  String json = "{";
  for (uint8_t mode = 0; mode < 4; ++mode) {
    bool automatic = mode >= 2;
    bool microphone = mode == 0 || mode == 3;
    if (mode) json += ',';
    json += "\"" + String(recordingModeCode(static_cast<RecordingMode>(mode))) + "\":{\"start\":" +
      String(playbackCueSetting(automatic, microphone, true)) + ",\"stop\":" +
      String(playbackCueSetting(automatic, microphone, false)) + "}";
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json + "}");
}

void saveRecordCues() {
  String body = server.arg("plain");
  if (body.length() > 512) { server.send(400, "text/plain", "Invalid recording cues"); return; }
  cJSON *json = cJSON_Parse(body.c_str());
  bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 4;
  uint16_t settings = 0;
  for (uint8_t mode = 0; mode < 4 && valid; ++mode) {
    cJSON *profile = cJSON_GetObjectItemCaseSensitive(json, recordingModeCode(static_cast<RecordingMode>(mode)));
    valid = cJSON_IsObject(profile) && cJSON_GetArraySize(profile) == 2;
    for (uint8_t event = 0; event < 2 && valid; ++event) {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(profile, event ? "stop" : "start");
      valid = cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 2 &&
        value->valuedouble == floor(value->valuedouble);
      if (valid) settings |= static_cast<uint16_t>(value->valueint) << (mode * 4 + event * 2);
    }
  }
  cJSON_Delete(json);
  if (!valid) { server.send(400, "text/plain", "Invalid recording cues"); return; }
  if (!playbackSaveCueSettings(settings)) { server.send(500, "text/plain", "Could not save recording cues"); return; }
  sendRecordCues();
}

void sendClock() {
  wifiManagerNotifyActivity();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", appClockJson());
}

void sendAutomaticRecording() {
  wifiManagerNotifyActivity(); server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", automaticRecordingJson());
}

void setAutomaticRecordingSettings() {
  String body = server.arg("plain");
  if (body.length() > 2048 || body.indexOf("\\u0000") >= 0) { server.send(400, "text/plain", "Invalid automatic settings"); return; }
  cJSON *json = cJSON_Parse(body.c_str());
  AutoRecordConfig config[2]; bool valid = cJSON_IsObject(json);
  for (uint8_t source = 0; source < 2 && valid; ++source) {
    cJSON *profile = cJSON_GetObjectItemCaseSensitive(json, source ? "microphone" : "radio");
    cJSON *threshold = cJSON_GetObjectItemCaseSensitive(profile, "threshold");
    cJSON *silence = cJSON_GetObjectItemCaseSensitive(profile, "silenceSeconds");
    valid = cJSON_IsNumber(threshold) && cJSON_IsNumber(silence) && threshold->valuedouble >= 1 &&
      threshold->valuedouble <= (source ? 32768 : 4095) && floor(threshold->valuedouble) == threshold->valuedouble &&
      silence->valuedouble >= 1 && silence->valuedouble <= 60 && floor(silence->valuedouble) == silence->valuedouble;
    if (valid) { config[source].threshold = threshold->valueint; config[source].silenceSeconds = silence->valueint; }
  }
  std::array<RecordingMode, 4> order;
  cJSON *priorities = cJSON_GetObjectItemCaseSensitive(json, "priorities");
  valid = valid && cJSON_IsArray(priorities) && cJSON_GetArraySize(priorities) == 4;
  for (uint8_t index = 0; index < 4 && valid; ++index) {
    cJSON *value = cJSON_GetArrayItem(priorities, index); order[index] = RecordingMode::none;
    if (cJSON_IsString(value)) for (uint8_t mode = 0; mode < 4; ++mode) {
      if (strcmp(value->valuestring, recordingModeCode(static_cast<RecordingMode>(mode))) == 0) order[index] = static_cast<RecordingMode>(mode);
    }
  }
  valid = valid && validRecordingPriorities(order); cJSON_Delete(json);
  if (!valid) { server.send(400, "text/plain", "Invalid automatic settings or priorities"); return; }
  if (!automaticSaveSettings(config[0], config[1], order)) { server.send(500, "text/plain", "Could not save automatic settings to NVS"); return; }
  displayWake(); sendAutomaticRecording();
}

void setLanguage() {
  String language = server.arg("language");
  if (!appLanguageSupported(language.c_str())) {
    server.send(400, "text/plain", "Unsupported language");
    return;
  }
  if (!appSetLanguage(language)) {
    server.send(500, "text/plain", "Could not save language to NVS");
    return;
  }
  displayWake();
  sendSettings();
}

void setClock() {
  wifiManagerNotifyActivity();
  String local = server.arg("local");
  if (local.length() != 16) { server.send(400, "text/plain", "Invalid local date/time"); return; }
  if (!appClockSetLocal(local)) { server.send(400, "text/plain", "Invalid date/time or NVS unavailable"); return; }
  sendClock();
}

void setPhoneClock() {
  wifiManagerNotifyActivity();
  String text = server.arg("epoch");
  if (text.length() != 10) { server.send(400, "text/plain", "Invalid UTC epoch"); return; }
  for (char digit : text) if (!isDigit(digit)) { server.send(400, "text/plain", "Invalid UTC epoch"); return; }
  uint64_t epoch = strtoull(text.c_str(), nullptr, 10);
  if (epoch < 1577836800ULL || epoch > 4102444799ULL) {
    server.send(400, "text/plain", "Invalid UTC epoch"); return;
  }
  if (!appClockSetPhoneEpoch(static_cast<time_t>(epoch))) {
    server.send(500, "text/plain", "Could not set phone time"); return;
  }
  sendClock();
}

void setClockZone() {
  wifiManagerNotifyActivity();
  String text = server.arg("zone");
  if (text.isEmpty() || text.length() > 2) { server.send(400, "text/plain", "Invalid timezone"); return; }
  for (char digit : text) if (!isDigit(digit)) { server.send(400, "text/plain", "Invalid timezone"); return; }
  uint8_t zone = static_cast<uint8_t>(text.toInt());
  if (zone > 28 || !appClockSetTimezone(zone)) { server.send(400, "text/plain", "Invalid timezone or NVS unavailable"); return; }
  sendClock();
}

void saveSettings() {
  wifiManagerNotifyActivity();
  auto parseMinutes = [](const String &value, uint16_t &minutes) {
    if (value.isEmpty() || value.length() > 3) return false;
    for (char digit : value) if (!isDigit(digit)) return false;
    minutes = static_cast<uint16_t>(value.toInt());
    return minutes <= 999;
  };
  uint16_t screenMinutes = 0;
  if (!parseMinutes(server.arg("screenOffMinutes"), screenMinutes)) {
    server.send(400, "text/plain", "screenOffMinutes must be an integer from 0 to 999");
    return;
  }
  String wifiValue = server.arg("wifiAutoOffMinutes");
  uint16_t wifiMinutes = wifiManagerAutoOffMinutes();
  if (!wifiValue.isEmpty() && !parseMinutes(wifiValue, wifiMinutes)) {
    server.send(400, "text/plain", "wifiAutoOffMinutes must be an integer from 0 to 999");
    return;
  }
  String totValue = server.arg("radioTotSeconds");
  uint16_t totSeconds = playbackRadioTotSeconds();
  if (!totValue.isEmpty() && (!parseMinutes(totValue, totSeconds) || totSeconds < 10 || totSeconds > 600)) {
    server.send(400, "text/plain", "radioTotSeconds must be an integer from 10 to 600");
    return;
  }
  if (!appSetScreenOffMinutes(screenMinutes)) {
    server.send(500, "text/plain", "Could not save screen settings to NVS");
    return;
  }
  if (!wifiValue.isEmpty() && !wifiManagerSetAutoOffMinutes(wifiMinutes)) {
    server.send(500, "text/plain", "Could not save Wi-Fi settings to NVS");
    return;
  }
  if (!totValue.isEmpty() && !playbackSetRadioTotSeconds(totSeconds)) {
    server.send(500, "text/plain", "Could not save radio TOT to NVS");
    return;
  }
  displayWake();
  server.sendHeader("Cache-Control", "no-store");
  String json = "{\"screenOffMinutes\":" + String(appScreenOffMinutes());
  json += ",\"wifiAutoOffMinutes\":" + String(wifiManagerAutoOffMinutes());
  json += ",\"radioTotSeconds\":" + String(playbackRadioTotSeconds()) + "}";
  server.send(200, "application/json", json);
}

void sendRecordings() {
  wifiManagerNotifyActivity();
  if (!appSdReady() || recorderState() != CaptureState::idle) {
    server.send(409, "text/plain", "SD unavailable or recording in progress");
    return;
  }
  uint32_t offset = max(0L, server.arg("offset").toInt());
  uint32_t total = 0;
  if (!visitRecordings([&total](const String &, size_t) { ++total; return true; })) {
    server.send(500, "text/plain", "Cannot read SD card");
    return;
  }
  const uint32_t end = offset < total ? total - offset : 0;
  const uint32_t start = end > 20 ? end - 20 : 0;
  String names[20];
  String paths[20];
  size_t sizes[20];
  uint32_t index = 0;
  uint32_t shown = 0;
  if (start < end && !visitRecordings([&](const String &path, size_t size) {
    if (index >= start && index < end) {
      paths[shown] = path;
      names[shown] = path.substring(path.lastIndexOf('/') + 1);
      sizes[shown++] = size;
    }
    return ++index < end;
  })) { server.send(500, "text/plain", "Cannot read SD card"); return; }
  String json = "{\"files\":[";
  for (uint32_t item = shown; item > 0; --item) {
    if (item < shown) json += ',';
    json += "{\"name\":\"" + names[item - 1] + "\",\"path\":\"" + paths[item - 1] +
            "\",\"bytes\":" + String(sizes[item - 1]) + "}";
  }
  json += offset + shown < total ? "],\"next\":" + String(offset + shown) + "}" : "],\"next\":null}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

bool safeSdPath(const String &path) {
  return managedPathValid(path.c_str());
}

void sendFiles() {
  wifiManagerNotifyActivity();
  String directoryPath = server.hasArg("dir") ? server.arg("dir") : "/";
  ManagedFilePage page;
  String sort = server.hasArg("sort") ? server.arg("sort") : "date";
  String order = server.hasArg("order") ? server.arg("order") : "desc";
  if (order != "asc" && order != "desc") { server.send(400, "text/plain", "Invalid sort order"); return; }
  if (!fileManagerList(directoryPath, sort, order == "desc", server.arg("filter"), server.arg("after"), 20, server.arg("revision"), page)) {
    int status = page.error.indexOf("changed") >= 0 || page.error.indexOf("recording") >= 0 ? 409 :
                 page.error.indexOf("not found") >= 0 ? 404 : 400;
    server.send(status, "text/plain", page.error); return;
  }
  cJSON *result = cJSON_CreateObject();
  cJSON *items = result ? cJSON_AddArrayToObject(result, "items") : nullptr;
  if (!items) {
    cJSON_Delete(result);
    server.send(500, "text/plain", "Cannot list SD card");
    return;
  }
  for (const auto &entry : page.items) {
    String name = entry.name.c_str();
    String path = directoryPath == "/" ? "/" + name : directoryPath + "/" + name;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", name.c_str()); cJSON_AddStringToObject(item, "path", path.c_str());
    cJSON_AddBoolToObject(item, "directory", entry.directory); cJSON_AddNumberToObject(item, "bytes", entry.bytes);
    cJSON_AddNumberToObject(item, "modified", entry.modified); cJSON_AddItemToArray(items, item);
  }
  if (!page.next.isEmpty()) cJSON_AddStringToObject(result, "next", page.next.c_str());
  else cJSON_AddNullToObject(result, "next");
  cJSON_AddStringToObject(result, "revision", page.revision.c_str()); cJSON_AddNumberToObject(result, "total", page.total);
  cJSON_AddNumberToObject(result, "capacity", SD_MMC.totalBytes()); cJSON_AddNumberToObject(result, "used", SD_MMC.usedBytes());
  char *json = cJSON_PrintUnformatted(result);
  if (json) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
    cJSON_free(json);
  } else {
    server.send(500, "text/plain", "Cannot list SD card");
  }
  cJSON_Delete(result);
}

void fileOperation() {
  wifiManagerNotifyActivity();
  String body = server.arg("plain");
  if (body.length() > 8192 || body.indexOf("\\u0000") >= 0) { server.send(400, "text/plain", "Invalid JSON request"); return; }
  cJSON *request = cJSON_Parse(body.c_str());
  if (!request || !cJSON_IsObject(request)) { cJSON_Delete(request); server.send(400, "text/plain", "Invalid JSON request"); return; }
  auto text = [request](const char *name) -> String { cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name); return cJSON_IsString(value) ? value->valuestring : ""; };
  String source = text("source"), destination = text("destination"), error;
  bool success = false; uint32_t id = 0;
  if (server.uri() == "/api/files/mkdir") success = fileManagerMkdir(text("path"), error);
  else if (server.uri() == "/api/files/rename") success = fileManagerRename(source, destination, error);
  else {
    String action = text("action");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(request, "bytes");
    double bytes = cJSON_IsNumber(size) ? size->valuedouble : 0;
    if ((action == "upload" && !cJSON_IsNumber(size)) || bytes < 0 || bytes > 0xffffffffULL || floor(bytes) != bytes) error = "Invalid file size";
    else if (action == "delete" && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "confirm"))) error = "Missing deletion confirmation";
    else id = fileManagerStartJob(action, source, destination, static_cast<uint64_t>(bytes), error);
    success = id != 0;
  }
  cJSON_Delete(request);
  if (!success) {
    int status = error.indexOf("busy") >= 0 || error.indexOf("exists") >= 0 ? 409 : error.indexOf("not found") >= 0 ? 404 : 400;
    server.send(status, "text/plain", error); return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(id ? 202 : 200, "application/json", id ? fileManagerJobJson(id) : "{\"ok\":true}");
}

bool removeSdTree(const String &path, uint8_t depth) {
  if (depth > 8) return false;
  File directory = SD_MMC.open(path, FILE_READ);
  if (!directory || !directory.isDirectory()) return false;
  File child;
  while ((child = directory.openNextFile())) {
    String childPath = path + "/" + fileName(child);
    bool nested = child.isDirectory();
    child.close();
    if (!safeSdPath(childPath) || !(nested ? removeSdTree(childPath, depth + 1) : SD_MMC.remove(childPath))) {
      directory.close();
      return false;
    }
    watchdogResetCurrentTask();
  }
  directory.close();
  return SD_MMC.rmdir(path);
}

void deleteSdEntry() {
  wifiManagerNotifyActivity();
  String path = server.arg("path");
  if (!safeSdPath(path) || path == "/" || server.arg("confirm") != "1") {
    server.send(400, "text/plain", "Invalid path or missing confirmation");
    return;
  }
  if (!appSdReady() || recorderState() != CaptureState::idle || playbackStatus().state != PlaybackState::idle || fileManagerBusy()) {
    server.send(409, "text/plain", "SD, recorder or playback busy");
    return;
  }
  File entry = SD_MMC.open(path, FILE_READ);
  if (!entry) { server.send(404, "text/plain", "File or folder not found"); return; }
  bool directory = entry.isDirectory();
  entry.close();
  bool removed = directory ? removeSdTree(path, 0) : SD_MMC.remove(path);
  if (!removed) { server.send(500, "text/plain", "Cannot remove file or folder"); return; }
  recorderSetRecordingsPresent(hasRecordings());
  server.send(200, "application/json", "{\"deleted\":true}");
}

bool walkSdFolder(const String &folder, const String &prefix, uint8_t depth,
                  const std::function<bool(File &, const String &)> &visit) {
  if (depth > 8) return false;
  File directory = SD_MMC.open(folder, FILE_READ);
  if (!directory || !directory.isDirectory()) return false;
  File child;
  while ((child = directory.openNextFile())) {
    String name = fileName(child);
    String path = folder + "/" + name;
    String relative = prefix + name;
    bool nested = child.isDirectory();
    bool valid = safeSdPath(path) && relative.length() <= 220;
    if (nested) child.close();
    if (valid) valid = nested ? walkSdFolder(path, relative + "/", depth + 1, visit) : visit(child, relative);
    child.close();
    if (!valid) { directory.close(); return false; }
  }
  directory.close();
  return true;
}

uint32_t zipCrc(File &file) {
  uint32_t crc = 0xffffffffU;
  uint8_t buffer[1024];
  while (file.available()) {
    size_t read = file.read(buffer, sizeof(buffer));
    if (!read) return 0;
    for (size_t index = 0; index < read; ++index) {
      crc ^= buffer[index];
      for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320U : 0);
    }
    watchdogResetCurrentTask();
  }
  return ~crc;
}

void zip16(WiFiClient &client, uint16_t value) {
  uint8_t bytes[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  client.write(bytes, sizeof(bytes));
}

void zip32(WiFiClient &client, uint32_t value) {
  zip16(client, value);
  zip16(client, value >> 16);
}

void zipName(WiFiClient &client, const String &name) {
  client.write(reinterpret_cast<const uint8_t *>(name.c_str()), name.length());
}

void downloadSdFolder() {
  wifiManagerNotifyActivity();
  String folder = server.arg("path");
  if (!safeSdPath(folder) || folder == "/") { server.send(400, "text/plain", "Invalid folder"); return; }
  if (!appSdReady() || recorderState() != CaptureState::idle || fileManagerBusy() || automaticAnyArmed()) {
    server.send(409, "text/plain", "SD or recorder busy"); return;
  }
  File directory = SD_MMC.open(folder, FILE_READ);
  if (!directory || !directory.isDirectory()) { server.send(404, "text/plain", "Folder not found"); return; }
  directory.close();
  String label = folder.substring(folder.lastIndexOf('/') + 1);
  uint64_t dataSize = 0;
  uint64_t centralSize = 0;
  uint32_t count = 0;
  bool valid = walkSdFolder(folder, label + "/", 0, [&](File &file, const String &name) {
    dataSize += 30ULL + name.length() + file.size();
    centralSize += 46ULL + name.length();
    ++count;
    return count <= 65535 && dataSize + centralSize + 22 <= 0xffffffffULL;
  });
  if (!valid) { server.send(413, "text/plain", "Folder too deep or ZIP too large"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + label + ".zip\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(dataSize + centralSize + 22);
  server.send(200, "application/zip", "");
  WiFiClient client = server.client();
  walkSdFolder(folder, label + "/", 0, [&](File &file, const String &name) {
    uint32_t size = file.size();
    uint32_t crc = zipCrc(file);
    if (!file.seek(0)) return false;
    zip32(client, 0x04034b50); zip16(client, 20); zip16(client, 0x0800);
    zip16(client, 0); zip16(client, 0); zip16(client, 0);
    zip32(client, crc); zip32(client, size); zip32(client, size);
    zip16(client, name.length()); zip16(client, 0); zipName(client, name);
    uint8_t buffer[1024];
    while (file.available() && client.connected()) {
      size_t read = file.read(buffer, sizeof(buffer));
      if (!read || client.write(buffer, read) != read) return false;
      watchdogResetCurrentTask();
    }
    return client.connected() != 0;
  });
  uint32_t offset = 0;
  if (client.connected()) walkSdFolder(folder, label + "/", 0, [&](File &file, const String &name) {
    uint32_t size = file.size();
    uint32_t crc = zipCrc(file);
    zip32(client, 0x02014b50); zip16(client, 20); zip16(client, 20); zip16(client, 0x0800);
    zip16(client, 0); zip16(client, 0); zip16(client, 0);
    zip32(client, crc); zip32(client, size); zip32(client, size);
    zip16(client, name.length()); zip16(client, 0); zip16(client, 0);
    zip16(client, 0); zip16(client, 0); zip32(client, 0); zip32(client, offset);
    zipName(client, name);
    offset += 30 + name.length() + size;
    return client.connected() != 0;
  });
  if (client.connected()) {
    zip32(client, 0x06054b50); zip16(client, 0); zip16(client, 0);
    zip16(client, count); zip16(client, count);
    zip32(client, centralSize); zip32(client, dataSize); zip16(client, 0);
  }
}

void sendSignal() {
  wifiManagerNotifyActivity();
  SignalStatus signal = recorderSignalStatus();
  String json = "{\"enabled\":" + String(signal.enabled ? "true" : "false");
  json += ",\"active\":" + String(signal.active ? "true" : "false");
  json += ",\"sequence\":" + String(signal.sequence);
  json += ",\"rms\":" + String(signal.rms);
  json += ",\"peak\":" + String(signal.peak);
  json += ",\"bias\":" + String(signal.bias);
  json += ",\"fault\":" + String(signal.fault) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setSignal(bool enabled) {
  if (!recorderAdcReady() || !recorderTaskReady()) {
    server.send(503, "text/plain", "ADC unavailable");
    return;
  }
  if (enabled && recorderMicSignalStatus().enabled) {
    server.send(409, "text/plain", "Microphone monitoring active");
    return;
  }
  recorderSetSignal(enabled);
  server.send(200, "text/plain", enabled ? "Monitoring" : "Stopped");
}

void sendMicSignal() {
  wifiManagerNotifyActivity();
  SignalStatus signal = recorderMicSignalStatus();
  String json = "{\"enabled\":" + String(signal.enabled ? "true" : "false");
  json += ",\"active\":" + String(signal.active ? "true" : "false");
  json += ",\"sequence\":" + String(signal.sequence);
  json += ",\"rms\":" + String(signal.rms);
  json += ",\"peak\":" + String(signal.peak);
  json += ",\"fault\":" + String(signal.fault) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setMicSignal(bool enabled) {
  wifiManagerNotifyActivity();
  if (!recorderMicReady() || !recorderTaskReady()) {
    server.send(503, "text/plain", "Microphone unavailable");
    return;
  }
  if (!recorderSetMicSignal(enabled)) {
    server.send(409, "text/plain", "Audio monitoring busy");
    return;
  }
  server.send(200, "text/plain", enabled ? "Monitoring" : "Stopped");
}

bool parseByteOffset(const String &text, uint32_t &value) {
  if (text.isEmpty()) return false;
  uint64_t parsed = 0;
  for (size_t index = 0; index < text.length(); ++index) {
    if (!isDigit(text[index])) return false;
    parsed = parsed * 10 + (text[index] - '0');
    if (parsed > 0xffffffffU) return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

const char *audioMime(const String &name) {
  String lower = name;
  lower.toLowerCase();
  if (lower.endsWith(".wav")) return "audio/wav";
  if (lower.endsWith(".mp3")) return "audio/mpeg";
  if (lower.endsWith(".m4a") || lower.endsWith(".mp4")) return "audio/mp4";
  if (lower.endsWith(".ogg") || lower.endsWith(".oga") || lower.endsWith(".opus")) return "audio/ogg";
  if (lower.endsWith(".flac")) return "audio/flac";
  if (lower.endsWith(".aac")) return "audio/aac";
  if (lower.endsWith(".webm")) return "audio/webm";
  return nullptr;
}

void sendFile() {
  wifiManagerNotifyActivity();
  String path = server.hasArg("path") ? server.arg("path") : String("/") + server.arg("name");
  if (!appSdReady() || recorderState() != CaptureState::idle || fileManagerBusy() || automaticAnyArmed()) {
    server.send(409, "text/plain", "Recording in progress or SD unavailable");
    return;
  }
  if (!safeSdPath(path) || path == "/") {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  String name = fileName(file);
  const char *mime = audioMime(name);
  if (!mime) mime = "application/octet-stream";
  uint32_t total = file.size();
  server.sendHeader("Accept-Ranges", "bytes");
  if (server.arg("download") == "1" || strcmp(mime, "application/octet-stream") == 0) {
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  }
  server.sendHeader("Cache-Control", "no-store");
  String range = server.header("Range");
  if (range.isEmpty()) {
    server.streamFile(file, mime);
    file.close();
    return;
  }
  int separator = range.indexOf('-');
  uint32_t start = 0;
  uint32_t end = total - 1;
  uint32_t amount = 0;
  bool valid = range.startsWith("bytes=") && separator >= 6 && range.indexOf(',', 6) == -1;
  if (valid && separator == 6) {
    valid = parseByteOffset(range.substring(separator + 1), amount) && amount > 0;
    if (valid) start = amount >= total ? 0 : total - amount;
  } else if (valid) {
    valid = parseByteOffset(range.substring(6, separator), start);
    if (valid && separator + 1 < range.length()) valid = parseByteOffset(range.substring(separator + 1), end);
  }
  if (!valid || start >= total || end < start) {
    server.sendHeader("Content-Range", "bytes */" + String(total));
    server.send(416, "text/plain", "Invalid byte range");
    return;
  }
  if (end >= total) end = total - 1;
  if (end - start >= 128 * 1024 && server.arg("download") != "1") end = start + 128 * 1024 - 1;
  server.sendHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(total));
  server.setContentLength(end - start + 1);
  server.send(206, mime, "");
  if (!file.seek(start)) return;
  auto &client = server.client();
  uint32_t remaining = end - start + 1;
  uint8_t buffer[1024];
  while (remaining && client.connected()) {
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t read = file.read(buffer, chunk);
    if (!read) break;
    size_t sent = 0;
    while (sent < read && client.connected()) {
      size_t count = client.write(buffer + sent, read - sent);
      if (!count) break;
      sent += count;
    }
    if (sent != read) break;
    remaining -= sent;
  }
  file.close();
}

void startSpeakerPlayback() {
  wifiManagerNotifyActivity();
  String targetArg = server.arg("target");
  if (targetArg != "" && targetArg != "speaker" && targetArg != "radio") {
    server.send(400, "text/plain", "Invalid playback target");
    return;
  }
  PlaybackTarget target = targetArg == "radio" ? PlaybackTarget::radio : PlaybackTarget::speaker;
  PlaybackStatus status = playbackStatus();
  if (!appSdReady() || (target == PlaybackTarget::radio ? !status.radioReady : !status.ready)) {
    server.send(503, "text/plain", "Audio output or SD unavailable");
    return;
  }
  if (!appCanStartPlayback()) {
    server.send(409, "text/plain", "Stop recording, playback or live audio first");
    return;
  }
  String path = server.arg("path");
  String lowerPath = path;
  lowerPath.toLowerCase();
  if (!safeSdPath(path) || path == "/" || !lowerPath.endsWith(".wav")) {
    server.send(400, "text/plain", "Invalid WAV path");
    return;
  }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "WAV not found");
    return;
  }
  uint32_t remaining = 0;
  bool supported = findPcmData(file, remaining);
  file.close();
  if (!supported) {
    server.send(415, "text/plain", "Only mono 16-bit 22050 Hz PCM WAV is supported");
    return;
  }
  playbackStart(path, target);
  server.send(200, "text/plain", target == PlaybackTarget::radio ? "Playing on radio DAC" : "Playing on ESP speaker");
}

void sendPlaybackWaveform() {
  wifiManagerNotifyActivity();
  String path = server.arg("path"), lower = path;
  lower.toLowerCase();
  if (!safeSdPath(path) || path == "/" || !lower.endsWith(".wav")) { server.send(400, "text/plain", "Invalid WAV path"); return; }
  if (!appSdReady() || recorderState() != CaptureState::idle || fileManagerBusy() || automaticAnyArmed() ||
      playbackGetState() != PlaybackState::idle) { server.send(409, "text/plain", "SD busy"); return; }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) { server.send(404, "text/plain", "WAV not found"); return; }
  uint32_t length = 0;
  if (!findPcmData(file, length)) { file.close(); server.send(415, "text/plain", "Unsupported PCM WAV"); return; }
  uint32_t start = file.position();
  String json = "{\"bytes\":" + String(length) + ",\"peaks\":[";
  json.reserve(800);
  uint8_t samples[2048];
  for (uint32_t bucket = 0; bucket < 128; ++bucket) {
    uint32_t position = start + (static_cast<uint64_t>(length) * bucket / 128 & ~1ULL);
    if (!file.seek(position)) { file.close(); server.send(500, "text/plain", "WAV scan failed"); return; }
    size_t count = file.read(samples, min(static_cast<uint32_t>(sizeof(samples)), start + length - position));
    uint16_t peak = 0;
    for (size_t index = 0; index + 1 < count; index += 2)
      peak = max(peak, static_cast<uint16_t>(abs(static_cast<int32_t>(static_cast<int16_t>(wav16(samples + index))))));
    if (bucket) json += ',';
    json += String(peak);
    if ((bucket & 7) == 7) { watchdogResetCurrentTask(); delay(1); }
  }
  file.close();
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void initWeb() {
  server.on("/radio", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kRadioPage);
  });
  server.on("/api/radio", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", radioProgrammingJson());
  });
  server.on("/api/radio/read", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (server.arg("confirm") != "1") {
      server.send(400, "text/plain", "Confirm verified wiring and disconnected PTT/audio with confirm=1");
      return;
    }
    if (!radioProgrammingStart()) {
      server.send(409, "text/plain", "Radio read or PTT already active");
      return;
    }
    server.send(202, "application/json", radioProgrammingJson());
  });
  server.on("/api/radio/schema", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", radioPatchSchemaJson());
  });
  server.on("/api/radio/write", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (server.arg("confirm") != "WRITE UV5R") { server.send(400, "text/plain", "WRITE UV5R confirmation required"); return; }
    String origin = server.header("Origin");
    if (!server.header("Content-Type").startsWith("application/json") ||
        (origin.length() && origin != "http://" + server.hostHeader())) {
      server.send(403, "text/plain", "Same-origin JSON request required"); return;
    }
    String error;
    if (!radioProgrammingWriteJson(server.arg("plain"), error)) { server.send(409, "text/plain", error); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send(202, "application/json", radioProgrammingJson(false));
  });
  server.on("/api/radio/image", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    size_t length = 0;
    const uint8_t *image = radioProgrammingImage(length);
    if (!image) { server.send(409, "text/plain", "No complete radio image"); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", "attachment; filename=uv5r-readonly.img");
    server.setContentLength(length);
    server.send(200, "application/octet-stream", "");
    server.sendContent(reinterpret_cast<const char *>(image), length);
  });
  server.on("/", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
  });
  server.on("/files", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kFilesPage);
  });
  server.on("/automatic-replay", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kAutomaticReplayPage);
  });
  server.on("/settings", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kSettingsPage);
  });
  server.on("/test", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kTestPage);
  });
  server.on("/dac-test", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kDacTestPage);
  });
  server.on("/api/settings", HTTP_GET, sendSettings);
  server.on("/api/settings", HTTP_POST, saveSettings);
  server.on("/api/performance", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", performanceJson());
  });
  server.on("/api/performance", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("enabled");
    if (value != "0" && value != "1") { server.send(400, "text/plain", "Invalid enabled value"); return; }
    if (!performanceSetEnabled(value == "1")) { server.send(500, "text/plain", "Cannot save performance setting to NVS"); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", performanceJson());
  });
  server.on("/api/performance/log", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("enabled");
    if (value != "0" && value != "1") { server.send(400, "text/plain", "Invalid enabled value"); return; }
    if (!performanceLogSetEnabled(value == "1")) {
      server.send(409, "text/plain", "Diagnostics, SD, or audio busy; log not started"); return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", performanceJson());
  });
  server.on("/api/performance/logs", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    if (!performanceLogSdIdle()) { server.send(409, "text/plain", "SD or audio busy"); return; }
    File root = SD_MMC.open(kPerformanceLogDirectory, FILE_READ);
    if (!root || !root.isDirectory()) { root.close(); server.send(503, "text/plain", "SD unavailable"); return; }
    String json = "{\"files\":[";
    uint8_t count = 0;
    uint32_t visited = 0;
    File file = root.openNextFile();
    while (file) {
      String name = file.name();
      String path = String(kPerformanceLogDirectory) + "/" + name.substring(name.lastIndexOf('/') + 1);
      if (!file.isDirectory() && performanceLogValidPath(path) && file.size() <= 256 * 1024) {
        if (count++) json += ',';
        json += "{\"path\":\"" + path + "\",\"bytes\":" + String(file.size()) + "}";
      }
      file.close();
      feedSystemWatchdog();
      if (++visited % 32 == 0) delay(1);
      if (count >= 32) break;
      file = root.openNextFile();
    }
    root.close();
    json += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.on("/api/performance/log", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    String path = server.hasArg("path") ? server.arg("path") : String(performanceLogPath());
    if (!performanceLogSdIdle()) {
      server.send(409, "text/plain", "No available diagnostics log or SD busy"); return;
    }
    if (!performanceLogValidPath(path)) { server.send(400, "text/plain", "Invalid log path"); return; }
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.isDirectory()) { file.close(); server.send(404, "text/plain", "Log not found"); return; }
    if (file.size() > 256 * 1024) { file.close(); server.send(413, "text/plain", "Log too large"); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(path.lastIndexOf('/') + 1) + "\"");
    server.streamFile(file, "application/x-ndjson");
    file.close();
  });
  server.on("/api/record-cues", HTTP_GET, sendRecordCues);
  server.on("/api/record-cues", HTTP_POST, saveRecordCues);
  server.on("/api/time", HTTP_GET, sendClock);
  server.on("/api/language", HTTP_POST, setLanguage);
  server.on("/api/time", HTTP_POST, setClock);
  server.on("/api/time/auto", HTTP_POST, setPhoneClock);
  server.on("/api/time/zone", HTTP_POST, setClockZone);
  server.on("/api/ptt-test", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", String("{\"active\":") + (pttTestActive() ? "true" : "false") +
                ",\"pulseMs\":" + String(kPttTestPulseMs) + "}");
  });
  server.on("/api/ptt-test/on", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!startPttTest()) { server.send(409, "text/plain", "PTT test already active"); return; }
    server.send(200, "text/plain", "PTT test pulse started");
  });
  server.on("/api/ptt-test/latch", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!latchPttTest()) { server.send(409, "text/plain", "PTT already active"); return; }
    server.send(200, "text/plain", "PTT relay on");
  });
  server.on("/api/ptt-test/off", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    stopPttTest();
    server.send(200, "text/plain", "PTT test off");
  });
  server.on("/api/wifi/status", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", wifiManagerStatusJson());
  });
  server.on("/api/wifi/scan", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!wifiManagerRequestScan()) {
      server.send(409, "text/plain", "Wi-Fi scan could not start");
      return;
    }
    server.send(202, "application/json", "{\"state\":\"scanning\"}");
  });
  server.on("/api/wifi/scan", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", wifiManagerScanJson());
  });
  server.on("/api/wifi/connect", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    String keepApValue = server.arg("keepApHost");
    bool keepApHost = keepApValue == "1" || keepApValue == "true" || keepApValue == "on";
    if (!wifiManagerConnect(ssid, password, keepApHost)) {
      server.send(400, "text/plain", "Invalid SSID or Wi-Fi change unavailable");
      return;
    }
    server.send(202, "application/json", "{\"state\":\"connecting\"}");
  });
  server.on("/api/wifi/forget", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!wifiManagerForget()) {
      server.send(500, "text/plain", "Could not forget saved Wi-Fi network");
      return;
    }
    server.send(200, "application/json", "{\"state\":\"ap\"}");
  });
  server.on("/api/display/wake", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!displayReady()) {
      server.send(503, "text/plain", "OLED unavailable");
      return;
    }
    displayWake();
    server.send(200, "text/plain", "Display awake");
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/play", HTTP_POST, startSpeakerPlayback);
  server.on("/api/play/waveform", HTTP_GET, sendPlaybackWaveform);
  server.on("/api/play/pause", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String paused = server.arg("paused");
    if (paused != "0" && paused != "1") { server.send(400, "text/plain", "Invalid pause setting"); return; }
    server.send(playbackPause(paused == "1") ? 200 : 409, "text/plain", "Playback pause");
  });
  server.on("/api/play/seek", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String text = server.arg("offset");
    if (text.isEmpty() || text.length() > 10) { server.send(400, "text/plain", "Invalid offset"); return; }
    for (char digit : text) if (!isDigit(digit)) { server.send(400, "text/plain", "Invalid offset"); return; }
    uint64_t offset = strtoull(text.c_str(), nullptr, 10);
    server.send(offset <= UINT32_MAX && playbackSeek(static_cast<uint32_t>(offset)) ? 200 : 409, "text/plain", "Playback seek");
  });
  server.on("/api/dac-tone", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String frequency = server.arg("frequency");
    String rate = server.arg("rate");
    String bits = server.arg("bits");
    String format = server.arg("format");
    String level = server.arg("level");
    String duration = server.arg("duration");
    if ((frequency != "400" && frequency != "1000" && frequency != "2000") ||
        (rate != "22050" && rate != "44100" && rate != "48000") ||
        (bits != "16" && bits != "32") || (format != "philips" && format != "msb") ||
        (level != "20" && level != "50" && level != "90") ||
        (duration != "10" && duration != "30" && duration != "60" && duration != "0")) {
      server.send(400, "text/plain", "Invalid DAC tone settings");
      return;
    }
    if (!appCanStartPlayback() || !playbackStatus().ready) {
      server.send(409, "text/plain", "Stop recording, playback or live audio first");
      return;
    }
    DacToneSettings settings = {static_cast<uint16_t>(frequency.toInt()), static_cast<uint32_t>(rate.toInt()),
                                static_cast<uint8_t>(bits.toInt()), static_cast<uint8_t>(level.toInt()), format == "msb",
                                static_cast<uint8_t>(duration.toInt())};
    if (!playbackStartTone(settings)) { server.send(409, "text/plain", "DAC tone unavailable"); return; }
    server.send(202, "text/plain", "DAC tone started");
  });
  server.on("/api/play/volume", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("value");
    if (value.isEmpty() || value.length() > 3) {
      server.send(400, "text/plain", "Volume must be 0-100");
      return;
    }
    for (char digit : value) {
      if (!isDigit(digit)) {
        server.send(400, "text/plain", "Volume must be 0-100");
        return;
      }
    }
    int volume = value.toInt();
    if (volume > 100) {
      server.send(400, "text/plain", "Volume must be 0-100");
      return;
    }
    playbackSetVolume(volume);
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/radio-volume", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("value");
    if (value.isEmpty() || value.length() > 3) {
      server.send(400, "text/plain", "Radio volume must be 0-100");
      return;
    }
    for (char digit : value) {
      if (!isDigit(digit)) {
        server.send(400, "text/plain", "Radio volume must be 0-100");
        return;
      }
    }
    int volume = value.toInt();
    if (volume > 100) {
      server.send(400, "text/plain", "Radio volume must be 0-100");
      return;
    }
    playbackSetRadioVolume(volume);
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/radio-boost", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("value");
    if (value != "1" && value != "2" && value != "4" && value != "8" && value != "16") {
      server.send(400, "text/plain", "Radio gain must be 1, 2, 4, 8 or 16");
      return;
    }
    playbackSetRadioBoost(value.toInt());
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/boost", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    String value = server.arg("value");
    if (value != "1" && value != "2" && value != "4") {
      server.send(400, "text/plain", "Boost must be 1, 2 or 4");
      return;
    }
    playbackSetBoost(value.toInt());
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/stop", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!playbackStop()) {
      server.send(409, "text/plain", "Speaker is idle");
      return;
    }
    server.send(200, "text/plain", "Stopping speaker");
  });
  server.on("/api/filter/on", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    recorderSetLowpass(true);
    server.send(200, "text/plain", "Low-pass enabled");
  });
  server.on("/api/filter/off", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    recorderSetLowpass(false);
    server.send(200, "text/plain", "Low-pass disabled");
  });
  server.on("/api/recordings", HTTP_GET, sendRecordings);
  server.on("/api/files", HTTP_GET, sendFiles);
  server.on("/api/auto-record", HTTP_GET, sendAutomaticRecording);
  server.on("/api/automatic-replay", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", automaticReplayRulesJson());
  });
  server.on("/api/automatic-replay/rules", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!server.header("Content-Type").startsWith("application/json") ||
        (server.header("Origin").length() && server.header("Origin") != "http://" + server.hostHeader())) {
      server.send(403, "text/plain", "Same-origin JSON required"); return;
    }
    String error;
    if (!automaticReplaySaveRule(server.arg("plain"), error)) {
      server.send(400, "text/plain", error); return;
    }
    server.send(200, "application/json", automaticReplayRulesJson());
  });
  server.on("/api/automatic-replay/activate", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!server.header("Content-Type").startsWith("application/json") ||
        (server.header("Origin").length() && server.header("Origin") != "http://" + server.hostHeader())) {
      server.send(403, "text/plain", "Same-origin JSON required"); return;
    }
    String id = server.arg("id"), enabled = server.arg("enabled");
    if (id.isEmpty() || id.length() > 10 || (enabled != "0" && enabled != "1")) {
      server.send(400, "text/plain", "Invalid replay rule state"); return;
    }
    for (char digit : id) if (!isDigit(digit)) {
      server.send(400, "text/plain", "Invalid replay rule id"); return;
    }
    String error;
    if (!automaticReplaySetRuleActive(static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 10)), enabled == "1", error)) {
      server.send(409, "text/plain", error); return;
    }
    server.send(200, "application/json", automaticReplayRulesJson());
  });
  server.on("/api/automatic-replay/stop", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    automaticReplayStop();
    server.send(200, "application/json", automaticReplayRulesJson());
  });
  server.on("/api/automatic-replay/arm", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (server.arg("confirm") != "MEASURED TOT" ||
        !server.header("Content-Type").startsWith("application/json") ||
        (server.header("Origin").length() && server.header("Origin") != "http://" + server.hostHeader())) {
      server.send(403, "text/plain", "Measured TOT and same-origin confirmation required"); return;
    }
    String error;
    if (!automaticReplayRequestArm(error)) { server.send(409, "text/plain", error); return; }
    displayWake();
    server.send(202, "application/json", automaticReplayRulesJson());
  });
  server.on("/api/auto-record/settings", HTTP_POST, setAutomaticRecordingSettings);
  server.on("/api/auto-record/arm", HTTP_POST, []() {
    String source = server.arg("source"), enabled = server.arg("enabled");
    if ((source != "radio" && source != "microphone") || (enabled != "0" && enabled != "1")) {
      server.send(400, "text/plain", "Invalid automatic source or state"); return;
    }
    if (!automaticSetArmed(source == "microphone", enabled == "1")) { server.send(409, "text/plain", "Cannot arm automatic recording"); return; }
    displayWake(); sendAutomaticRecording();
  });
  server.on("/api/files/mkdir", HTTP_POST, fileOperation);
  server.on("/api/files/rename", HTTP_POST, fileOperation);
  server.on("/api/files/jobs", HTTP_POST, fileOperation);
  server.on("/api/files/jobs/status", HTTP_GET, []() {
    wifiManagerNotifyActivity(); server.sendHeader("Cache-Control", "no-store");
    String json = fileManagerJobJson(server.arg("id").toInt());
    server.send(json == "{}" ? 404 : 200, "application/json", json);
  });
  server.on("/api/files/jobs/cancel", HTTP_POST, []() {
    server.send(fileManagerCancelJob(server.arg("id").toInt()) ? 200 : 409, "application/json", "{}");
  });
  server.on("/api/files/delete", HTTP_POST, deleteSdEntry);
  server.on("/api/files/download", HTTP_GET, downloadSdFolder);
  server.on("/api/signal", HTTP_GET, sendSignal);
  server.on("/api/spectrum", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    const String source = server.arg("source");
    if (source != "radio" && source != "microphone") { server.send(400, "text/plain", "Invalid source"); return; }
    uint8_t samples[kSpectrumPcmBytes];
    if (!recorderSpectrumSnapshot(source == "microphone" ? CaptureSource::microphone : CaptureSource::adc, samples)) {
      server.send(204); return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(sizeof(samples));
    server.send(200, "application/octet-stream", "");
    server.sendContent(reinterpret_cast<const char *>(samples), sizeof(samples));
  });
  server.on("/api/adc/raw", HTTP_GET, []() {
    wifiManagerNotifyActivity();
    String pin = server.arg("gpio");
    if (pin.length() != 1 && pin.length() != 2) { server.send(400, "text/plain", "Invalid ADC GPIO"); return; }
    for (char digit : pin) {
      if (!isDigit(digit)) { server.send(400, "text/plain", "Invalid ADC GPIO"); return; }
    }
    uint16_t samples[kAdcProbeSamples];
    AdcProbeResult result = recorderReadAdcSamples(static_cast<uint8_t>(pin.toInt()), samples);
    if (result != AdcProbeResult::ok) {
      int code = result == AdcProbeResult::invalidPin ? 400 : result == AdcProbeResult::busy ? 409 :
                 result == AdcProbeResult::unavailable ? 503 : 500;
      server.send(code, "text/plain", result == AdcProbeResult::busy ? "ADC busy: stop recording, live audio or signal monitoring" :
                  result == AdcProbeResult::invalidPin ? "Invalid ADC GPIO" : "ADC unavailable");
      return;
    }
    String json = "{\"gpio\":" + pin + ",\"samples\":[";
    json.reserve(800);
    for (size_t index = 0; index < kAdcProbeSamples; ++index) {
      if (index) json += ',';
      json += String(samples[index]);
    }
    json += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.on("/api/signal/off", HTTP_POST, []() { wifiManagerNotifyActivity(); setSignal(false); });
  server.on("/api/signal/on", HTTP_POST, []() { wifiManagerNotifyActivity(); setSignal(true); });
  server.on("/api/mic/signal", HTTP_GET, sendMicSignal);
  server.on("/api/mic/signal/off", HTTP_POST, []() { setMicSignal(false); });
  server.on("/api/mic/signal/on", HTTP_POST, []() { setMicSignal(true); });
  server.on("/file", HTTP_GET, sendFile);
  server.on("/api/start", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStartRecording()) {
      bool started = automaticCurrentMode() == RecordingMode::manualRadio;
      server.send(started ? 200 : 202, "text/plain", started ? recorderName() : "Recording request queued");
    } else server.send(409, "text/plain", automaticCurrentMode() != RecordingMode::none ? "Higher priority recording active" :
                       recorderError()[0] ? recorderError() : "SD, ADC or recorder unavailable");
  });
  server.on("/api/mic/start", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStartMicRecording()) {
      bool started = automaticCurrentMode() == RecordingMode::manualMic;
      server.send(started ? 200 : 202, "text/plain", started ? recorderName() : "Recording request queued");
    } else server.send(409, "text/plain", automaticCurrentMode() != RecordingMode::none ? "Higher priority recording active" :
                       "SD, I2S microphone or recorder unavailable, or audio busy");
  });
  server.on("/api/stop", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStopRecording()) server.send(200, "text/plain", "Stopping");
    else server.send(409, "text/plain", "Not recording");
  });
  server.onNotFound([]() { wifiManagerNotifyActivity(); server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"Range", "Content-Type", "Origin"};
  server.collectHeaders(headers, 3);
  audioSocket.onEvent(onAudioSocket);
  routesReady = true;
}

void webStartServices() {
  if (!routesReady || wifiReady) return;
  server.begin();
  audioSocket.begin();
  fileManagerStartServices();
  wifiReady = true;
}

void webStopServices() {
  if (!wifiReady) return;
  server.close();
  audioSocket.close();
  fileManagerStopServices();
  resetLiveAudio();
  wifiReady = false;
}

void pollWeb() {
  if (!wifiReady) return;
  server.handleClient();
  pollFileManager();
  audioSocket.loop();
  sendLiveFrames();
}

