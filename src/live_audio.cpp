#include "live_audio.h"

#include <atomic>

static QueueHandle_t liveFrames = nullptr;
static std::atomic<int> liveClient{-1};
static std::atomic<bool> liveCaptureFault{false};

void initLiveAudio() {
  liveFrames = xQueueCreate(8, sizeof(AudioFrame));
}

bool liveAudioReady() {
  return liveFrames != nullptr;
}

int liveAudioClient() {
  return liveClient.load();
}

bool connectLiveAudio(int client) {
  if (!liveFrames || liveClient.load() >= 0) return false;
  xQueueReset(liveFrames);
  liveCaptureFault.store(false);
  liveClient.store(client);
  return true;
}

bool disconnectLiveAudio(int client) {
  if (liveClient.load() != client) return false;
  resetLiveAudio();
  return true;
}

void resetLiveAudio() {
  liveClient.store(-1);
  if (liveFrames) xQueueReset(liveFrames);
}

void faultLiveAudio() {
  liveCaptureFault.store(true);
}

bool takeLiveAudioFault() {
  return liveCaptureFault.exchange(false);
}

bool popLiveAudio(AudioFrame &frame) {
  return liveFrames && xQueueReceive(liveFrames, &frame, 0) == pdTRUE;
}

void queueLiveAudio(const uint8_t *samples, size_t length) {
  if (liveClient.load() < 0 || liveFrames == nullptr) return;
  for (size_t offset = 0; offset < length; offset += kLiveFrameBytes) {
    AudioFrame frame = {};
    frame.length = min(kLiveFrameBytes, length - offset);
    memcpy(frame.samples, samples + offset, frame.length);
    xQueueSend(liveFrames, &frame, 0);
  }
}