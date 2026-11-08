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
#include "watchdog.h"

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
constexpr uint16_t kLogMaxFiles = 128;
bool logEnabled = false;
bool logPreferred = true;
bool autoLogPending = false;
bool bootLogCountReady = false;
bool logDirectoryReady = false;
uint16_t bootLogCount = 0;
char logPath[96] = {};
char logError[64] = {};
uint32_t logBytes = 0;
uint32_t logSamples = 0;
uint32_t logSkipped = 0;
uint32_t logBuildUs = 0;
uint32_t logWriteUs = 0;
uint32_t lastLogAt = 0;

bool sdAvailableForLog() {
  return appSdReady() && recorderState() == CaptureState::idle &&
  playbackGetState() == PlaybackState::idle && !fileManagerBusy() &&
  !automaticAnyArmed() && !radioProgrammingBusy();
}

bool validLegacyLogPath(const String &path) {
  if (!path.startsWith("/perf-") || !path.endsWith(".txt") || path.length() > 63) return false;
  for (size_t index = 1; index < path.length(); ++index) {
    char ch = path[index];
    if (!isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '.') return false;
  }
  return true;
}

bool validLogPath(const String &path) {
  return path.startsWith(String(kPerformanceLogDirectory) + "/") &&
         path.length() < sizeof(logPath) &&
         validLegacyLogPath(path.substring(sizeof(kPerformanceLogDirectory) - 1));
}

void stopLog(const char *error);

bool prepareLogDirectory() {
  if (logDirectoryReady) return true;
  if (!SD_MMC.exists(kPerformanceLogDirectory) && !SD_MMC.mkdir(kPerformanceLogDirectory)) {
    stopLog("Cannot create SD log folder");
    return false;
  }
  File directory = SD_MMC.open(kPerformanceLogDirectory, FILE_READ);
  bool ready = directory && directory.isDirectory();
  directory.close();
  if (!ready) { stopLog("Invalid SD log folder"); return false; }
  while (true) {
    File root = SD_MMC.open("/", FILE_READ);
    if (!root || !root.isDirectory()) {
      root.close(); stopLog("Cannot scan old SD logs"); return false;
    }
    String oldPath;
    File file = root.openNextFile();
    while (file) {
      String name = file.name();
      String path = "/" + name.substring(name.lastIndexOf('/') + 1);
      if (!file.isDirectory() && validLegacyLogPath(path)) oldPath = path;
      file.close();
      if (oldPath.length()) break;
      feedSystemWatchdog();
      file = root.openNextFile();
    }
    root.close();
    if (!oldPath.length()) { logDirectoryReady = true; return true; }
    String destination = String(kPerformanceLogDirectory) + oldPath;
    if (SD_MMC.exists(destination) || !SD_MMC.rename(oldPath, destination)) {
      stopLog("Cannot move old SD log");
      return false;
    }
    feedSystemWatchdog();
  }
}

uint16_t logFileCount() {
  File root = SD_MMC.open(kPerformanceLogDirectory, FILE_READ);
  if (!root || !root.isDirectory()) { root.close(); return kLogMaxFiles; }
  uint16_t count = 0;
  uint32_t visited = 0;
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    String path = String(kPerformanceLogDirectory) + "/" + name.substring(name.lastIndexOf('/') + 1);
    if (!file.isDirectory() && validLogPath(path)) ++count;
    file.close();
    if (count >= kLogMaxFiles) break;
    feedSystemWatchdog();
    if (++visited % 32 == 0) delay(1);
    file = root.openNextFile();
  }
  root.close();
  return count;
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
  logPreferred = preferences.getBool("perfLogEnabled", true);
  autoLogPending = enabled.load() && logPreferred;
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
    autoLogPending = value && logPreferred;
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
bool performanceLogSdIdle() { return sdAvailableForLog(); }
bool performanceLogValidPath(const String &path) { return validLogPath(path); }
void performancePrimeLogCount() {
  if (!appSdReady()) return;
  if (!prepareLogDirectory()) { autoLogPending = false; return; }
  if (autoLogPending) {
    bootLogCount = logFileCount();
    bootLogCountReady = true;
  }
}

bool performanceLogSetEnabled(bool value) {
  if (value && !logEnabled && (!enabled.load() || !sdAvailableForLog())) {
    stopLog("Diagnostics or SD busy"); return false;
  }
  Preferences preferences;
  if (!preferences.begin("radio-recorder", false)) return false;
  bool saved = logPreferred == value || preferences.putBool("perfLogEnabled", value) == sizeof(uint8_t);
  preferences.end();
  if (!saved) return false;
  logPreferred = value;
  autoLogPending = false;
  if (!value) { logEnabled = false; return true; }
  if (logEnabled) return true;
  if (!prepareLogDirectory()) return false;
  uint16_t existingLogs = bootLogCountReady ? bootLogCount : logFileCount();
  bootLogCountReady = false;
  if (existingLogs >= kLogMaxFiles) { stopLog("128 SD logs reached; remove older logs"); return false; }
  time_t now = time(nullptr);
  tm utc = {};
  if (now >= 1577836800) gmtime_r(&now, &utc);
  char base[80];
  char stamp[24];
  if (now >= 1577836800) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
  else snprintf(stamp, sizeof(stamp), "boot-%lu", static_cast<unsigned long>(millis()));
  snprintf(base, sizeof(base), "%s/perf-%s-v%s", kPerformanceLogDirectory, stamp, appFirmwareVersion().c_str());
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
  if (autoLogPending && sdAvailableForLog()) {
    autoLogPending = false;
    performanceLogSetEnabled(true);
  }
  if (!logEnabled || !enabled.load() || static_cast<uint32_t>(millis() - lastLogAt) < kLogIntervalMs) return;
  lastLogAt = millis();
  if (!sdAvailableForLog()) { ++logSkipped; return; }
  uint32_t buildStartedUs = micros();
  time_t now = time(nullptr);
  tm utc = {};
  char timestamp[24] = "unknown";
  if (now >= 1577836800) {
    gmtime_r(&now, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  String line = String("{\"timestampUtc\":\"") + timestamp + "\",\"firmware\":\"" +
                appFirmwareVersion() + "\",\"metrics\":" + performanceJson() + "}\n";
  logBuildUs = micros() - buildStartedUs;
  if (logBytes + line.length() > kLogMaxBytes) { stopLog("Log reached 256 KiB limit"); return; }
  uint32_t writeStartedUs = micros();
  File file = SD_MMC.open(logPath, FILE_APPEND);
  if (!file) { stopLog("Cannot append SD log"); return; }
  size_t written = file.write(reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
  file.close();
  logWriteUs = micros() - writeStartedUs;
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
  json += ",\"logBuildUs\":" + String(logBuildUs);
  json += ",\"logWriteUs\":" + String(logWriteUs);
  json += ",\"taskCount\":" + String(uxTaskGetNumberOfTasks());
  json += ",\"loopStackFreeMinimum\":" + String(uxTaskGetStackHighWaterMark(nullptr));
  json += ",\"recorderStackFreeMinimum\":" + String(recorderStackFreeMinimum());
  json += ",\"speakerStackFreeMinimum\":" + String(playbackStackFreeMinimum());
  json += ",\"lastFileWorkerStackFreeMinimum\":" + String(fileManagerLastStackFreeMinimum());
  json += ",\"adcOverflows\":" + String(recorderAdcOverflowCount());
  json += ",\"liveAudioDroppedFrames\":" + String(liveAudioDroppedFrames());
  json += ",\"internalFree\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"internalTotal\":" + String(heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"internalMinimumFree\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"internalLargestBlock\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  json += ",\"psramFree\":" + String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  json += ",\"psramTotal\":" + String(heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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