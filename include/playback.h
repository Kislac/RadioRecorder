#pragma once

#include <Arduino.h>

enum class PlaybackState { idle, starting, playing };
enum class PlaybackTarget { speaker, radio };
enum class PlaybackCue { startup, recordingStarted, recordingStopped };

struct PlaybackStatus {
  PlaybackState state;
  bool ready;
  bool radioReady;
  PlaybackTarget target;
  uint8_t volume;
  uint8_t boost;
  uint8_t radioVolume;
  uint32_t bytesPlayed;
  uint32_t bytesTotal;
  String path;
  String error;
};

void initPlayback(bool sdReady);
PlaybackState playbackGetState();
PlaybackStatus playbackStatus();
void playbackStart(const String &path, PlaybackTarget target = PlaybackTarget::speaker);
void playbackPlayCue(PlaybackCue cue);
bool playbackStop();
void playbackSetVolume(uint8_t volume);
void playbackSetRadioVolume(uint8_t volume);
void playbackSetBoost(uint8_t boost);
void playbackPollSettings();