#pragma once

#include <Arduino.h>

constexpr size_t kLiveFrameBytes = 1024;

struct AudioFrame {
  uint16_t length;
  uint8_t samples[kLiveFrameBytes];
};

void initLiveAudio();
bool liveAudioReady();
int liveAudioClient();
bool connectLiveAudio(int client);
bool disconnectLiveAudio(int client);
void resetLiveAudio();
void faultLiveAudio();
bool takeLiveAudioFault();
bool popLiveAudio(AudioFrame &frame);
void queueLiveAudio(const uint8_t *samples, size_t length);