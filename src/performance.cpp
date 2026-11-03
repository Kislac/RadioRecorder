#include "performance.h"

#include <Preferences.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <nvs.h>
#include "recorder.h"
#include "playback.h"
#include "file_manager.h"
#include "live_audio.h"

namespace {
std::atomic<bool> enabled{true};
uint32_t lastLoopUs = 0;
uint32_t maxLoopGapUs = 0;
uint32_t slowLoops = 0;
constexpr uint32_t kSlowLoopUs = 100000;
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
    lastLoopUs = 0;
    maxLoopGapUs = 0;
    slowLoops = 0;
  }
  return saved;
}

void performanceLoopTick() {
  if (!enabled.load()) return;
  uint32_t now = micros();
  if (lastLoopUs) {
    uint32_t gap = now - lastLoopUs;
    if (gap > maxLoopGapUs) maxLoopGapUs = gap;
    if (gap > kSlowLoopUs) ++slowLoops;
  }
  lastLoopUs = now;
}

String performanceJson() {
  if (!enabled.load()) return "{\"enabled\":false}";
  String json;
  json.reserve(450);
  json = "{\"enabled\":true,\"uptimeMs\":" + String(millis());
  json += ",\"loopMaxGapUs\":" + String(maxLoopGapUs);
  json += ",\"loopGapsOver100ms\":" + String(slowLoops);
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