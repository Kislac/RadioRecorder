#include <Arduino.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <cJSON.h>
#include "web_server.h"
#include "web_pages.h"
#include "wav.h"
#include "playback.h"
#include "recorder.h"
#include "live_audio.h"
#include "app_control.h"

static WebServer server(80);
static WebSocketsServer audioSocket(81);
static bool wifiReady = false;

bool webReady() { return wifiReady; }

static void onAudioSocket(uint8_t client, WStype_t event, uint8_t *, size_t) {
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
  if (name.length() != 12) return false;
  String upper = name;
  upper.toUpperCase();
  if (!upper.startsWith("REC") || !upper.endsWith(".WAV")) return false;
  for (int index = 3; index < 8; ++index) if (!isDigit(name[index])) return false;
  return true;
}

String fileName(const File &file) {
  String name = file.name();
  return name.substring(name.lastIndexOf('/') + 1);
}

bool hasRecordings() {
  File directory = SD_MMC.open("/");
  if (!directory) return false;
  File entry;
  while ((entry = directory.openNextFile())) {
    bool found = !entry.isDirectory() && isRecordingFile(fileName(entry)) && entry.size() >= 44;
    entry.close();
    if (found) return true;
  }
  return false;
}

void sendStatus() {
  CaptureState state = recorderState();
  bool ready = appSdReady() && recorderAdcReady() && recorderTaskReady();
  String json = "{\"ready\":" + String(ready ? "true" : "false");
  json += ",\"version\":\"" + appFirmwareVersion() + "\"";
  json += ",\"demoSeconds\":" + String(appDemoTimeSeconds());
  json += ",\"audioGpio\":" + String(kAudioGpio);
  json += ",\"lowpass\":" + String(recorderLowpass() ? "true" : "false");
  json += ",\"meterReady\":" + String(recorderAdcReady() && recorderTaskReady() ? "true" : "false");
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

void sendRecordings() {
  if (!appSdReady() || recorderState() != CaptureState::idle) {
    server.send(409, "text/plain", "SD unavailable or recording in progress");
    return;
  }
  uint32_t offset = max(0L, server.arg("offset").toInt());
  File directory = SD_MMC.open("/");
  if (!directory) {
    server.send(500, "text/plain", "Cannot read SD card");
    return;
  }
  uint32_t total = 0;
  File entry;
  while ((entry = directory.openNextFile())) {
    if (!entry.isDirectory() && isRecordingFile(fileName(entry)) && entry.size() >= 44) ++total;
    entry.close();
  }
  directory.close();

  const uint32_t end = offset < total ? total - offset : 0;
  const uint32_t start = end > 20 ? end - 20 : 0;
  String names[20];
  size_t sizes[20];
  uint32_t index = 0;
  uint32_t shown = 0;
  if (start < end) {
    directory = SD_MMC.open("/");
    if (!directory) {
      server.send(500, "text/plain", "Cannot read SD card");
      return;
    }
    while ((entry = directory.openNextFile())) {
      String name = fileName(entry);
      if (!entry.isDirectory() && isRecordingFile(name) && entry.size() >= 44) {
        if (index >= start && index < end) {
          names[shown] = name;
          sizes[shown++] = entry.size();
        }
        if (++index >= end) {
          entry.close();
          break;
        }
      }
      entry.close();
    }
    directory.close();
  }
  String json = "{\"files\":[";
  for (uint32_t item = shown; item > 0; --item) {
    if (item < shown) json += ',';
    json += "{\"name\":\"" + names[item - 1] + "\",\"bytes\":" + String(sizes[item - 1]) + "}";
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
  File entry;
  while ((entry = directory.openNextFile())) {
    if (index >= offset && shown < 20) {
      String name = fileName(entry);
      String path = directoryPath == "/" ? "/" + name : directoryPath + "/" + name;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", name.c_str());
      cJSON_AddStringToObject(item, "path", path.c_str());
      cJSON_AddBoolToObject(item, "directory", entry.isDirectory());
      cJSON_AddNumberToObject(item, "bytes", entry.isDirectory() ? 0 : entry.size());
      cJSON_AddItemToArray(items, item);
      ++shown;
    } else if (index >= offset + 20) {
      moreFiles = true;
      entry.close();
      break;
    }
    ++index;
    entry.close();
  }
  directory.close();
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

void sendSignal() {
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
  recorderSetSignal(enabled);
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
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("RadioRecorder")) {
    Serial.println("Cannot start Wi-Fi AP");
    return;
  }
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
  });
  server.on("/files", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kFilesPage);
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/play", HTTP_POST, startSpeakerPlayback);
  server.on("/api/play/volume", HTTP_POST, []() {
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
    String value = server.arg("value");
    if (value != "1" && value != "2" && value != "4") {
      server.send(400, "text/plain", "Boost must be 1, 2 or 4");
      return;
    }
    playbackSetBoost(value.toInt());
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/stop", HTTP_POST, []() {
    if (!playbackStop()) {
      server.send(409, "text/plain", "Speaker is idle");
      return;
    }
    server.send(200, "text/plain", "Stopping speaker");
  });
  server.on("/api/filter/on", HTTP_POST, []() {
    recorderSetLowpass(true);
    server.send(200, "text/plain", "Low-pass enabled");
  });
  server.on("/api/filter/off", HTTP_POST, []() {
    recorderSetLowpass(false);
    server.send(200, "text/plain", "Low-pass disabled");
  });
  server.on("/api/recordings", HTTP_GET, sendRecordings);
  server.on("/api/files", HTTP_GET, sendFiles);
  server.on("/api/signal", HTTP_GET, sendSignal);
  server.on("/api/signal/on", HTTP_POST, []() { setSignal(true); });
  server.on("/api/signal/off", HTTP_POST, []() { setSignal(false); });
  server.on("/file", HTTP_GET, sendFile);
  server.on("/api/start", HTTP_POST, []() {
    if (appStartRecording()) server.send(200, "text/plain", recorderName());
    else server.send(409, "text/plain", recorderError()[0] ? recorderError() : "SD, ADC or recorder unavailable");
  });
  server.on("/api/wifi-test", HTTP_POST, []() {
    if (!appStartWifiTest()) {
      server.send(409, "text/plain", "Stop live audio and other recordings first");
      return;
    }
    server.send(200, "text/plain", recorderName());
  });
  server.on("/api/stop", HTTP_POST, []() {
    if (appStopRecording()) server.send(200, "text/plain", "Stopping");
    else server.send(409, "text/plain", "Not recording");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"Range"};
  server.collectHeaders(headers, 1);
  server.begin();
  audioSocket.onEvent(onAudioSocket);
  audioSocket.begin();
  wifiReady = true;
}

void toggleWifi() {
  if (appWifiTestActive()) {
    Serial.println("Wi-Fi zajteszt alatt nem kapcsolhato");
    return;
  }
  if (wifiReady) {
    server.close();
    audioSocket.close();
    resetLiveAudio();
    if (!WiFi.mode(WIFI_OFF)) {
      server.begin();
      audioSocket.begin();
      Serial.println("Wi-Fi kikapcsolasa sikertelen");
      return;
    }
    wifiReady = false;
    Serial.println("Wi-Fi kikapcsolva; a felvetel folytatodik");
  } else if (WiFi.mode(WIFI_AP) && WiFi.softAP("RadioRecorder")) {
    server.begin();
    audioSocket.begin();
    wifiReady = true;
    Serial.print("Wi-Fi bekapcsolva: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi bekapcsolasa sikertelen");
  }
}

bool webDisableForNoiseTest() {
  if (!WiFi.mode(WIFI_OFF)) return false;
  wifiReady = false;
  return true;
}

void pollWeb() {
  if (!wifiReady) return;
  server.handleClient();
  audioSocket.loop();
  sendLiveFrames();
}

