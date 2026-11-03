#include "automatic_record.h"
#include "app_control.h"
#include "file_manager.h"
#include "live_audio.h"
#include "playback.h"
#include <Preferences.h>
#include <atomic>
#include <cJSON.h>
#include <esp_timer.h>
#include <cstring>

namespace {
SemaphoreHandle_t controlMutex = nullptr;
AutoRecordConfig configs[2];
AutoDetector detectors[2];
String lastFiles[2], errors[2], blockedReason;
uint64_t lastTimes[2] = {};
std::array<RecordingMode, 4> priorities = {RecordingMode::manualMic, RecordingMode::manualRadio, RecordingMode::autoRadio, RecordingMode::autoMic};
std::array<bool, 2> manualRequested = {};
RecordingMode current = RecordingMode::none;
std::atomic<bool> monitorRadio{false}, monitorMic{false}, anyArmed{false};
std::atomic<int8_t> previewSource{-1};
bool settingsReady = false;

struct StoredSettings {
  uint32_t magic = 0x41555431;
  uint16_t thresholds[2] = {100, 100};
  uint8_t silence[2] = {3, 3};
  uint8_t order[4] = {0, 1, 2, 3};
};

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
void lock() { xSemaphoreTake(controlMutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(controlMutex); }
bool isAutomatic(RecordingMode mode) { return mode == RecordingMode::autoRadio || mode == RecordingMode::autoMic; }
bool isMicrophone(RecordingMode mode) { return mode == RecordingMode::manualMic || mode == RecordingMode::autoMic; }
size_t rank(RecordingMode mode) { for (size_t index = 0; index < priorities.size(); ++index) if (priorities[index] == mode) return index; return 4; }
}

const char *recordingModeCode(RecordingMode mode) {
  switch (mode) {
    case RecordingMode::manualMic: return "manualMic";
    case RecordingMode::manualRadio: return "manualRadio";
    case RecordingMode::autoRadio: return "autoRadio";
    case RecordingMode::autoMic: return "autoMic";
    default: return "none";
  }
}

const char *recordingModeLabel(RecordingMode mode) {
  switch (mode) {
    case RecordingMode::manualMic: return "Manual mic";
    case RecordingMode::manualRadio: return "Manual radio";
    case RecordingMode::autoRadio: return "Auto radio";
    case RecordingMode::autoMic: return "Auto mic";
    default: return "None";
  }
}

bool initAutomaticRecording() {
  controlMutex = xSemaphoreCreateMutex();
  if (!controlMutex) return false;
  StoredSettings stored;
  Preferences preferences;
  settingsReady = preferences.begin("auto-record", true);
  if (settingsReady) {
    if (preferences.getBytesLength("settings") == sizeof(stored)) preferences.getBytes("settings", &stored, sizeof(stored));
    preferences.end();
  }
  std::array<RecordingMode, 4> order;
  for (size_t index = 0; index < 4; ++index) order[index] = static_cast<RecordingMode>(stored.order[index]);
  AutoRecordConfig radio{stored.thresholds[0], stored.silence[0]}, mic{stored.thresholds[1], stored.silence[1]};
  if (stored.magic == 0x41555431 && validAutoConfig(radio, false) && validAutoConfig(mic, true) && validRecordingPriorities(order)) {
    configs[0] = radio; configs[1] = mic; priorities = order;
  }
  return true;
}

bool automaticSaveSettings(const AutoRecordConfig &radio, const AutoRecordConfig &mic,
                           const std::array<RecordingMode, 4> &order) {
  if (!controlMutex || !validAutoConfig(radio, false) || !validAutoConfig(mic, true) || !validRecordingPriorities(order)) return false;
  StoredSettings stored;
  stored.thresholds[0] = radio.threshold; stored.thresholds[1] = mic.threshold;
  stored.silence[0] = radio.silenceSeconds; stored.silence[1] = mic.silenceSeconds;
  for (size_t index = 0; index < 4; ++index) stored.order[index] = static_cast<uint8_t>(order[index]);
  lock(); bool same = configs[0].threshold == radio.threshold && configs[0].silenceSeconds == radio.silenceSeconds &&
    configs[1].threshold == mic.threshold && configs[1].silenceSeconds == mic.silenceSeconds && priorities == order; unlock();
  if (same && settingsReady) return true;
  Preferences preferences;
  if (!preferences.begin("auto-record", false)) return false;
  size_t written = preferences.putBytes("settings", &stored, sizeof(stored)); preferences.end();
  if (written != sizeof(stored)) return false;
  lock(); configs[0] = radio; configs[1] = mic; priorities = order;
  detectors[0].aboveCount = detectors[1].aboveCount = 0; settingsReady = true; unlock();
  return true;
}

bool automaticAnyArmed() { return anyArmed.load(); }
bool automaticMonitorRadio() { return monitorRadio.load(); }
bool automaticMonitorMic() { return monitorMic.load(); }
void automaticPauseMonitoring() { monitorRadio.store(false); monitorMic.store(false); recorderNotify(); }

void automaticSetPreviewSource(int8_t source) {
  if (source < -1 || source > 1) source = -1;
  if (previewSource.exchange(source) != source) recorderNotify();
}

void automaticInputStarted(bool microphone) {
  if (!controlMutex) return;
  lock(); auto &detector = detectors[microphone ? 1 : 0];
  detector.arm(detector.armed, nowMs()); detector.rms = 0;
  unlock();
}

bool automaticSetArmed(bool microphone, bool enabled) {
  if (!controlMutex) return false;
  uint8_t source = microphone ? 1 : 0;
  if (enabled && liveAudioClient() >= 0) {
    lock(); errors[source] = "Live audio active"; unlock(); return false;
  }
  if (enabled && (!appSdReady() || !(microphone ? recorderMicReady() : recorderAdcReady()) || !recorderTaskReady())) {
    lock(); errors[source] = "SD or audio input unavailable"; unlock(); return false;
  }
  lock(); detectors[source].arm(enabled, nowMs()); errors[source] = "";
  anyArmed.store(detectors[0].armed || detectors[1].armed); unlock();
  pollAutomaticRecording(); recorderNotify(); return true;
}

void automaticInputFault(bool microphone, const char *error) {
  if (!controlMutex) return;
  lock(); uint8_t source = microphone ? 1 : 0;
  errors[source] = error; detectors[source].arm(false, nowMs());
  anyArmed.store(detectors[0].armed || detectors[1].armed); unlock();
}

void automaticObserve(bool microphone, uint16_t rms) {
  if (!controlMutex) return;
  uint8_t source = microphone ? 1 : 0;
  if (playbackCueActive()) {
    lock(); detectors[source].hasSample = false; detectors[source].aboveCount = 0;
    detectors[source].sampledAt = nowMs();
    if (current == (microphone ? RecordingMode::autoMic : RecordingMode::autoRadio)) detectors[source].lastSoundAt = nowMs();
    unlock(); return;
  }
  lock(); detectors[source].observe(rms, nowMs(), configs[source], current == (microphone ? RecordingMode::autoMic : RecordingMode::autoRadio)); unlock();
}

bool automaticRequestManual(CaptureSource source) {
  if (!controlMutex || !appSdReady() || fileManagerBusy() || playbackGetState() != PlaybackState::idle ||
      !(source == CaptureSource::microphone ? recorderMicReady() : recorderAdcReady()) ||
      (source == CaptureSource::microphone && liveAudioClient() >= 0)) return false;
  RecordingMode mode = source == CaptureSource::microphone ? RecordingMode::manualMic : RecordingMode::manualRadio;
  lock();
  if (current != RecordingMode::none && (current == mode || rank(mode) > rank(current))) { unlock(); return false; }
  for (uint8_t index = 0; index < 2; ++index) {
    RecordingMode pending = static_cast<RecordingMode>(index);
    if (manualRequested[index] && (pending == mode || rank(pending) < rank(mode))) { unlock(); return false; }
  }
  if (current == RecordingMode::none) for (uint8_t source = 0; source < 2; ++source) {
    RecordingMode automatic = source ? RecordingMode::autoMic : RecordingMode::autoRadio;
    if (detectors[source].ready(nowMs()) && rank(automatic) < rank(mode)) { unlock(); return false; }
  }
  manualRequested = {}; manualRequested[static_cast<uint8_t>(mode)] = true; unlock();
  pollAutomaticRecording();
  lock(); bool accepted = current == mode || manualRequested[static_cast<uint8_t>(mode)]; unlock();
  return accepted;
}

bool automaticStopCurrent() {
  if (!controlMutex) return stopRecording();
  lock(); bool pending = manualRequested[0] || manualRequested[1]; manualRequested = {};
  if (isAutomatic(current)) detectors[isMicrophone(current) ? 1 : 0].arm(false, nowMs());
  anyArmed.store(detectors[0].armed || detectors[1].armed); unlock();
  return stopRecording() || pending;
}

void pollAutomaticRecording() {
  if (!controlMutex) return;
  uint64_t now = nowMs();
  bool resources = appSdReady() && !fileManagerBusy() && !playbackFileActive() && liveAudioClient() < 0;
  CaptureState capture = recorderState();
  lock();
  blockedReason = !appSdReady() ? "SD unavailable" : fileManagerBusy() ? "File operation busy" :
    playbackFileActive() ? "Playback active" : liveAudioClient() >= 0 ? "Live audio active" : "";
  if (capture == CaptureState::idle && current != RecordingMode::none) {
    if (isAutomatic(current)) {
      uint8_t source = isMicrophone(current) ? 1 : 0;
      if (recorderFinalized()) { lastFiles[source] = recorderName(); lastTimes[source] = recorderElapsedMs(); }
      detectors[source].arm(detectors[source].armed, now);
      if (recorderError()[0]) { errors[source] = recorderError(); detectors[source].arm(false, now); }
    } else manualRequested[static_cast<uint8_t>(current)] = false;
    current = RecordingMode::none;
  }
  std::array<bool, 4> requested = {manualRequested[0], manualRequested[1], false, false};
  for (uint8_t source = 0; source < 2; ++source) {
    RecordingMode mode = source ? RecordingMode::autoMic : RecordingMode::autoRadio;
    bool active = current == mode && capture != CaptureState::idle;
    if (resources && detectors[source].armed && now - detectors[source].sampledAt > (detectors[source].hasSample ? 2000ULL : 5000ULL)) {
      errors[source] = detectors[source].hasSample ? "Audio signal timed out" : "Audio input did not produce samples";
      detectors[source].arm(false, now);
    }
    requested[static_cast<uint8_t>(mode)] = resources && detectors[source].armed &&
      (active ? !detectors[source].silent(now, configs[source]) : detectors[source].ready(now));
    if (!resources) { detectors[source].hasSample = false; detectors[source].aboveCount = 0; detectors[source].armedAt = now; detectors[source].sampledAt = now; }
  }
  anyArmed.store(detectors[0].armed || detectors[1].armed);
  bool radioNeeded = resources && (detectors[0].armed || previewSource.load() == 0);
  bool micNeeded = resources && (detectors[1].armed || previewSource.load() == 1);
  bool monitoringChanged = monitorRadio.exchange(radioNeeded) != radioNeeded;
  monitoringChanged = (monitorMic.exchange(micNeeded) != micNeeded) || monitoringChanged;
  if (monitoringChanged) recorderNotify();
  RecordingMode desired = chooseRecordingMode(priorities, requested), previous = current;
  if (capture != CaptureState::idle && desired != current && current != RecordingMode::none) {
    if (!isAutomatic(current)) manualRequested[static_cast<uint8_t>(current)] = false;
    unlock(); stopRecording(); return;
  }
  if (capture != CaptureState::idle || desired == RecordingMode::none) { unlock(); return; }
  current = desired;
  if (isAutomatic(desired)) detectors[isMicrophone(desired) ? 1 : 0].lastSoundAt = now;
  unlock();
  if (!recorderStart(appSdReady(), isMicrophone(desired) ? CaptureSource::microphone : CaptureSource::adc, isAutomatic(desired))) {
    lock(); current = previous;
    if (isAutomatic(desired)) { uint8_t source = isMicrophone(desired) ? 1 : 0; errors[source] = "Cannot start recording"; detectors[source].arm(false, now); }
    else manualRequested[static_cast<uint8_t>(desired)] = false;
    unlock();
  }
  recorderNotify();
}

AutomaticRecordStatus automaticStatus(bool microphone) {
  AutomaticRecordStatus status;
  if (!controlMutex) return status;
  uint8_t source = microphone ? 1 : 0;
  lock(); status.armed = detectors[source].armed; status.rms = detectors[source].rms; status.config = configs[source];
  status.recording = current == (microphone ? RecordingMode::autoMic : RecordingMode::autoRadio) && recorderState() != CaptureState::idle;
  status.elapsedMs = status.recording ? recorderElapsedMs() : 0;
  status.lastElapsedMs = lastTimes[source]; status.lastFile = lastFiles[source]; status.error = errors[source];
  status.blocked = blockedReason;
  if (status.armed && current != RecordingMode::none && !status.recording) status.blocked = "Higher priority recording active";
  unlock(); return status;
}

std::array<RecordingMode, 4> automaticPriorities() {
  if (!controlMutex) return priorities;
  lock(); auto result = priorities; unlock(); return result;
}

RecordingMode automaticCurrentMode() {
  if (!controlMutex) return RecordingMode::none;
  lock(); auto result = current; unlock(); return result;
}

String automaticRecordingJson() {
  cJSON *json = cJSON_CreateObject(); if (!json) return "{}";
  cJSON_AddStringToObject(json, "activeMode", recordingModeCode(automaticCurrentMode()));
  cJSON_AddStringToObject(json, "activeFile", recorderState() != CaptureState::idle ? recorderName() : "");
  for (bool microphone : {false, true}) {
    auto status = automaticStatus(microphone);
    cJSON *item = cJSON_AddObjectToObject(json, microphone ? "microphone" : "radio");
    cJSON_AddBoolToObject(item, "armed", status.armed); cJSON_AddBoolToObject(item, "recording", status.recording);
    cJSON_AddNumberToObject(item, "rms", status.rms); cJSON_AddNumberToObject(item, "threshold", status.config.threshold);
    cJSON_AddNumberToObject(item, "silenceSeconds", status.config.silenceSeconds);
    cJSON_AddNumberToObject(item, "elapsedMs", status.elapsedMs); cJSON_AddNumberToObject(item, "lastElapsedMs", status.lastElapsedMs);
    cJSON_AddStringToObject(item, "lastFile", status.lastFile.c_str()); cJSON_AddStringToObject(item, "error", status.error.c_str());
    cJSON_AddStringToObject(item, "blocked", status.blocked.c_str());
  }
  cJSON *order = cJSON_AddArrayToObject(json, "priorities");
  for (auto mode : automaticPriorities()) cJSON_AddItemToArray(order, cJSON_CreateString(recordingModeCode(mode)));
  char *text = cJSON_PrintUnformatted(json); String result = text ? text : "{}";
  if (text) cJSON_free(text);
  cJSON_Delete(json);
  return result;
}