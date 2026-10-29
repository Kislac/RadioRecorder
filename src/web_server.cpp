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
#include "display.h"
#include "wifi_manager.h"
#include "ptt_test.h"
#include "watchdog.h"
#include "radio_programming.h"

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

bool visitRecordings(const std::function<bool(const String &, size_t)> &visit) {
  File directory = SD_MMC.open("/");
  if (!directory) return false;
  File entry;
  while ((entry = directory.openNextFile())) {
    String name = fileName(entry);
    if (entry.isDirectory() && isDatedFolder(name)) {
      String folder = "/" + name;
      File dated = SD_MMC.open(folder);
      File child;
      while (dated && (child = dated.openNextFile())) {
        if (!child.isDirectory() && child.size() >= 44 && isRecordingFile(fileName(child)) &&
            !visit(folder + "/" + fileName(child), child.size())) {
          child.close(); dated.close(); entry.close(); directory.close();
          return true;
        }
        child.close();
      }
      dated.close();
    } else if (!entry.isDirectory() && entry.size() >= 44 && isRecordingFile(name) &&
               !visit("/" + name, entry.size())) {
      entry.close(); directory.close();
      return true;
    }
    entry.close();
  }
  directory.close();
  return true;
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
  json += ",\"screenOffMinutes\":" + String(appScreenOffMinutes());
  json += ",\"wifi\":" + wifiManagerStatusJson();
  json += ",\"demoSeconds\":" + String(appDemoTimeSeconds());
  json += ",\"clock\":" + appClockJson();
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
  json += ",\"speakerVolume\":" + String(speaker.volume);
  json += ",\"speakerBoost\":" + String(speaker.boost);
  json += ",\"playback\":\"" + String(playback == PlaybackState::idle ? "idle" : playback == PlaybackState::starting ? "starting" : "playing") + "\"";
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
  json += ",\"wifiAutoOffMinutes\":" + String(wifiManagerAutoOffMinutes());
  json += ",\"wifi\":" + wifiManagerStatusJson() + "}";
  server.send(200, "application/json", json);
}

void sendClock() {
  wifiManagerNotifyActivity();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", appClockJson());
}

void setClock() {
  wifiManagerNotifyActivity();
  String local = server.arg("local");
  if (local.length() != 16) { server.send(400, "text/plain", "Invalid local date/time"); return; }
  if (!appClockSetLocal(local)) { server.send(400, "text/plain", "Invalid date/time or NVS unavailable"); return; }
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
  if (!appSetScreenOffMinutes(screenMinutes)) {
    server.send(500, "text/plain", "Could not save screen settings to NVS");
    return;
  }
  if (!wifiValue.isEmpty() && !wifiManagerSetAutoOffMinutes(wifiMinutes)) {
    server.send(500, "text/plain", "Could not save Wi-Fi settings to NVS");
    return;
  }
  displayWake();
  server.sendHeader("Cache-Control", "no-store");
  String json = "{\"screenOffMinutes\":" + String(appScreenOffMinutes());
  json += ",\"wifiAutoOffMinutes\":" + String(wifiManagerAutoOffMinutes()) + "}";
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
  if (path.isEmpty() || path.length() > 220 || path[0] != '/') return false;
  if (path == "/") return true;
  int segmentStart = 1;
  for (size_t index = 1; index <= path.length(); ++index) {
    if (index < path.length() && static_cast<uint8_t>(path[index]) < 32) return false;
    if (index == path.length() || path[index] == '/') {
      String segment = path.substring(segmentStart, index);
      if (segment.isEmpty() || segment == "." || segment == "..") return false;
      segmentStart = index + 1;
    } else if (path[index] == '\\' || path[index] == ':' || path[index] == '"' || path[index] == 127) {
      return false;
    }
  }
  return true;
}

void sendFiles() {
  wifiManagerNotifyActivity();
  if (!appSdReady() || recorderState() != CaptureState::idle) {
    server.send(409, "text/plain", "SD unavailable or recording in progress");
    return;
  }
  String directoryPath = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!safeSdPath(directoryPath)) {
    server.send(400, "text/plain", "Invalid directory");
    return;
  }
  File directory = SD_MMC.open(directoryPath, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    server.send(404, "text/plain", "Directory not found");
    return;
  }
  directory.close();
  uint32_t offset = max(0L, server.arg("offset").toInt());
  cJSON *result = cJSON_CreateObject();
  cJSON *items = result ? cJSON_AddArrayToObject(result, "items") : nullptr;
  if (!items) {
    cJSON_Delete(result);
    server.send(500, "text/plain", "Cannot list SD card");
    return;
  }
  uint32_t index = 0;
  uint32_t shown = 0;
  bool moreFiles = false;
  for (uint8_t pass = 0; pass < 2 && !moreFiles; ++pass) {
    directory = SD_MMC.open(directoryPath, FILE_READ);
    if (!directory) {
      cJSON_Delete(result);
      server.send(500, "text/plain", "Cannot list SD card");
      return;
    }
    File entry;
    while ((entry = directory.openNextFile())) {
      bool folder = entry.isDirectory();
      if (folder != (pass == 0)) { entry.close(); continue; }
      if (index++ < offset) { entry.close(); continue; }
      if (shown == 20) {
        moreFiles = true;
        entry.close();
        break;
      }
      String name = fileName(entry);
      String path = directoryPath == "/" ? "/" + name : directoryPath + "/" + name;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", name.c_str());
      cJSON_AddStringToObject(item, "path", path.c_str());
      cJSON_AddBoolToObject(item, "directory", folder);
      cJSON_AddNumberToObject(item, "bytes", folder ? 0 : entry.size());
      cJSON_AddItemToArray(items, item);
      ++shown;
      entry.close();
    }
    directory.close();
  }
  if (moreFiles) cJSON_AddNumberToObject(result, "next", offset + shown);
  else cJSON_AddNullToObject(result, "next");
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
  if (!appSdReady() || recorderState() != CaptureState::idle || playbackStatus().state != PlaybackState::idle) {
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
  if (!appSdReady() || recorderState() != CaptureState::idle) {
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
  if (!appSdReady() || recorderState() != CaptureState::idle) {
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
  if (!appSdReady() || !playbackStatus().ready) {
    server.send(503, "text/plain", "Speaker or SD unavailable");
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
  playbackStart(path);
  server.send(200, "text/plain", "Playing on ESP speaker");
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
  server.on("/api/settings", HTTP_GET, sendSettings);
  server.on("/api/settings", HTTP_POST, saveSettings);
  server.on("/api/time", HTTP_GET, sendClock);
  server.on("/api/time", HTTP_POST, setClock);
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
  server.on("/api/files/delete", HTTP_POST, deleteSdEntry);
  server.on("/api/files/download", HTTP_GET, downloadSdFolder);
  server.on("/api/signal", HTTP_GET, sendSignal);
  server.on("/api/signal/off", HTTP_POST, []() { wifiManagerNotifyActivity(); setSignal(false); });
  server.on("/api/signal/on", HTTP_POST, []() { wifiManagerNotifyActivity(); setSignal(true); });
  server.on("/api/mic/signal", HTTP_GET, sendMicSignal);
  server.on("/api/mic/signal/off", HTTP_POST, []() { setMicSignal(false); });
  server.on("/api/mic/signal/on", HTTP_POST, []() { setMicSignal(true); });
  server.on("/file", HTTP_GET, sendFile);
  server.on("/api/start", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStartRecording()) server.send(200, "text/plain", recorderName());
    else server.send(409, "text/plain", recorderError()[0] ? recorderError() : "SD, ADC or recorder unavailable");
  });
  server.on("/api/mic/start", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStartMicRecording()) server.send(200, "text/plain", recorderName());
    else server.send(409, "text/plain", "SD, I2S microphone or recorder unavailable, or audio busy");
  });
  server.on("/api/wifi-test", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (!appStartWifiTest()) {
      server.send(409, "text/plain", "Stop live audio and other recordings first");
      return;
    }
    server.send(200, "text/plain", recorderName());
  });
  server.on("/api/stop", HTTP_POST, []() {
    wifiManagerNotifyActivity();
    if (appStopRecording()) server.send(200, "text/plain", "Stopping");
    else server.send(409, "text/plain", "Not recording");
  });
  server.onNotFound([]() { wifiManagerNotifyActivity(); server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"Range"};
  server.collectHeaders(headers, 1);
  audioSocket.onEvent(onAudioSocket);
  routesReady = true;
}

void webStartServices() {
  if (!routesReady || wifiReady) return;
  server.begin();
  audioSocket.begin();
  wifiReady = true;
}

void webStopServices() {
  if (!wifiReady) return;
  server.close();
  audioSocket.close();
  resetLiveAudio();
  wifiReady = false;
}

bool webDisableForNoiseTest() {
  appClockStopSync();
  webStopServices();
  if (!WiFi.mode(WIFI_OFF)) return false;
  return true;
}

void pollWeb() {
  if (!wifiReady) return;
  server.handleClient();
  audioSocket.loop();
  sendLiveFrames();
}

