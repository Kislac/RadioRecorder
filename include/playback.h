#pragma once

#include <Arduino.h>

enum class PlaybackState { idle, starting, playing };

struct PlaybackStatus {
  PlaybackState state;
  bool ready;
  uint8_t volume;
  uint8_t boost;
  String path;
  String error;
};

void initPlayback(bool sdReady);
PlaybackState playbackGetState();
PlaybackStatus playbackStatus();
void playbackStart(const String &path);
bool playbackStop();
void playbackSetVolume(uint8_t volume);
void playbackSetBoost(uint8_t boost);