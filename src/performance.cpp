#include "performance.h"

#include <Preferences.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <SD_MMC.h>
#include <time.h>
#include "app_control.h"
#include "recorder.h"
#include "playback.h"
#include "file_manager.h"
#include "live_audio.h"
#include "automatic_record.h"
#include "radio_programming.h"

namespace {
std::atomic<bool> enabled{true};
struct Timing {
  uint32_t current = 0;
  uint32_t peak = 0;
  uint64_t total = 0;
  uint64_t count = 0;
  void add(uint32_t value) {
    current = value;
    if (value > peak) peak = value;
    total += value;
    ++count;
  }
  uint32_t average() const { return count ? total / count : 0; }
};
Timing loopTiming, displayTiming;
uint32_t loopStartedUs = 0;
uint32_t lastLoopUs = 0;
uint32_t maxLoopGapUs = 0;
uint32_t slowLoops = 0;
constexpr uint32_t kSlowLoopUs = 100000;
constexpr uint32_t kLogIntervalMs = 60000;
constexpr uint32_t kLogMaxBytes = 256 * 1024;
bool logEnabled = false;
char logPath[64] = {};
char logError[64] = {};
uint32_t logBytes = 0;
uint32_t logSamples = 0;
uint32_t logSkipped = 0;
uint32_t lastLogAt = 0;

bool sdAvailableForLog() {
  return appSdReady() && recorderState() == CaptureState::idle &&
  playbackGetState() == PlaybackState::idle && !fileManagerBusy() &&
  !automaticAnyArmed() && !radioProgrammingBusy();
}

void stopLog(const char *error) {
  logEnabled = false;
  snprintf(logError, sizeof(logError), "%s", error);
}
}

bool performanceInit() {
  Preferences preferences;
  if (!preferences.begin("radio-recorder", true)) return false;
  enabled.store(preferences.getBool("perfEnabled", true));
  preferences.end();
  return true;
}

bool performanceEnabled() { return enabled.load(); }

bool performanceSetEnabled(bool value) {
  if (value == enabled.load()) return true;
  Preferences preferences;
  if (!preferences.begin("radio-recorder", false)) return false;
  bool saved = preferences.putBool("perfEnabled", value) == sizeof(uint8_t);
  preferences.end();
  if (saved) {
    enabled.store(value);
    if (!value) logEnabled = false;
    loopTiming = {};
    displayTiming = {};
    loopStartedUs = 0;
    lastLoopUs = 0;
    maxLoopGapUs = 0;
    slowLoops = 0;
  }
  return saved;
}

void performanceLoopStart() {
  if (!enabled.load()) return;
  uint32_t now = micros();
  if (lastLoopUs) {
    uint32_t gap = now - lastLoopUs;
    if (gap > maxLoopGapUs) maxLoopGapUs = gap;
    if (gap > kSlowLoopUs) ++slowLoops;
  }
  lastLoopUs = now;
  loopStartedUs = now;
}

void performanceLoopTick() {
  if (enabled.load() && loopStartedUs) loopTiming.add(micros() - loopStartedUs);
}

void performanceDisplaySample(uint32_t durationUs) {
  if (enabled.load()) displayTiming.add(durationUs);
}

bool performanceLogEnabled() { return logEnabled; }
const char *performanceLogPath() { return logPath; }

bool performanceLogSetEnabled(bool value) {
  if (!value) { logEnabled = false; return true; }
  if (logEnabled) return true;
  if (!enabled.load() || !sdAvailableForLog()) { stopLog("Diagnostics or SD busy"); return false; }
  time_t now = time(nullptr);
  tm utc = {};
  if (now >= 1577836800) gmtime_r(&now, &utc);
  char base[48];
  char stamp[24];
  if (now >= 1577836800) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
  else snprintf(stamp, sizeof(stamp), "boot-%lu", static_cast<unsigned long>(millis()));
  snprintf(base, sizeof(base), "/perf-%s-v%s", stamp, appFirmwareVersion().c_str());
  for (uint8_t index = 0; index < 16; ++index) {
    snprintf(logPath, sizeof(logPath), "%s-%02u.txt", base, index);
    if (SD_MMC.exists(logPath)) continue;
    File file = SD_MMC.open(logPath, FILE_WRITE);
    if (!file) break;
    file.close();
    logEnabled = true;
    logError[0] = '\0';
    logBytes = logSamples = logSkipped = 0;
    lastLogAt = millis() - kLogIntervalMs;
    return true;
  }
  logPath[0] = '\0';
  stopLog("Cannot create a unique SD log");
  return false;
}

void performanceLogPoll() {
  if (!logEnabled || !enabled.load() || static_cast<uint32_t>(millis() - lastLogAt) < kLogIntervalMs) return;
  lastLogAt = millis();
  if (!sdAvailableForLog()) { ++logSkipped; return; }
  time_t now = time(nullptr);
  tm utc = {};
  char timestamp[24] = "unknown";
  if (now >= 1577836800) {
    gmtime_r(&now, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  String line = String("{\"timestampUtc\":\"") + timestamp + "\",\"firmware\":\"" +
                appFirmwareVersion() + "\",\"metrics\":" + performanceJson() + "}\n";
  if (logBytes + line.length() > kLogMaxBytes) { stopLog("Log reached 256 KiB limit"); return; }
  File file = SD_MMC.open(logPath, FILE_APPEND);
  if (!file) { stopLog("Cannot append SD log"); return; }
  size_t written = file.write(reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
  file.close();
  if (written != line.length()) { stopLog("Incomplete SD log write"); return; }
  logBytes += written;
  ++logSamples;
}

String performanceJson() {
  if (!enabled.load()) return "{\"enabled\":false}";
  String json;
  json.reserve(850);
  json = "{\"enabled\":true,\"uptimeMs\":" + String(millis());
  json += ",\"loopMaxGapUs\":" + String(maxLoopGapUs);
  json += ",\"loopGapsOver100ms\":" + String(slowLoops);
  json += ",\"loopCurrentUs\":" + String(loopTiming.current);
  json += ",\"loopAverageUs\":" + String(loopTiming.average());
  json += ",\"loopPeakUs\":" + String(loopTiming.peak);
  json += ",\"loopSamples\":" + String(loopTiming.count);
  json += ",\"displayCurrentUs\":" + String(displayTiming.current);
  json += ",\"displayAverageUs\":" + String(displayTiming.average());
  json += ",\"displayPeakUs\":" + String(displayTiming.peak);
  json += ",\"displaySamples\":" + String(displayTiming.count);
  json += ",\"logEnabled\":" + String(logEnabled ? "true" : "false");
  json += ",\"logPath\":\"" + String(logPath) + "\"";
  json += ",\"logError\":\"" + String(logError) + "\"";
  json += ",\"logBytes\":" + String(logBytes);
  json += ",\"logSamples\":" + String(logSamples);
  json += ",\"logSkippedBusy\":" + String(logSkipped);
  json += ",\"taskCount\":" + String(uxTaskGetNumberOfTasks());
  json += ",\"loopStackFreeMinimum\":" + String(uxTaskGetStackHighWaterMark(nullptr));
  json += ",\"recorderStackFreeMinimum\":" + String(recorderStackFreeMinimum());
  json += ",\"speakerStackFreeMinimum\":" + String(playbackStackFreeMinimum());
  json += ",\"lastFileWorkerStackFreeMinimum\":" + String(fileManagerLastStackFreeMinimum());
  json += ",\"adcOverflows\":" + String(recorderAdcOverflowCount());
  json += ",\"liveAudioDroppedFrames\":" + String(liveAudioDroppedFrames());
  json += ",\"internalFree\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"internalMinimumFree\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"internalLargestBlock\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"psramFree\":" + String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  json += ",\"psramMinimumFree\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  json += ",\"psramLargestBlock\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  nvs_stats_t stats = {};
  if (nvs_get_stats("nvs", &stats) == ESP_OK) {
    json += ",\"nvsTotalEntries\":" + String(stats.total_entries);
    json += ",\"nvsUsedEntries\":" + String(stats.used_entries);
    json += ",\"nvsFreeEntries\":" + String(stats.free_entries);
  }
  json += '}';
  return json;
}