#pragma once
#include <Arduino.h>

uint32_t playbackStackFreeMinimum();

enum class PlaybackState { idle, starting, playing };
enum class PlaybackTarget { speaker, radio };
enum class PlaybackCue { startup, recordingStarted, recordingStopped, autoStarted, autoStopped };

struct DacToneSettings {
  uint16_t frequency;
  uint32_t sampleRate;
  uint8_t bits;
  uint8_t level;
  bool leftJustified;
};

struct PlaybackStatus {
  PlaybackState state;
  bool ready;
  bool radioReady;
  PlaybackTarget target;
  uint8_t volume;
  uint8_t boost;
  uint8_t radioVolume;
  uint8_t radioBoost;
  uint32_t bytesPlayed;
  uint32_t bytesTotal;
  bool paused;
  String path;
  String error;
};

void initPlayback(bool sdReady);
PlaybackState playbackGetState();
PlaybackStatus playbackStatus();
void playbackStart(const String &path, PlaybackTarget target = PlaybackTarget::speaker);
bool playbackStartTone(const DacToneSettings &settings);
void playbackPlayCue(PlaybackCue cue);
void playbackRecordCue(bool automatic, bool microphone, bool started);
uint8_t playbackCueSetting(bool automatic, bool microphone, bool started);
bool playbackSaveCueSettings(uint16_t settings);
bool playbackCueActive();
bool playbackFileActive();
bool playbackStop();
bool playbackPause(bool paused);
bool playbackSeek(uint32_t offset);
void playbackSetVolume(uint8_t volume);
void playbackSetRadioVolume(uint8_t volume);
void playbackSetRadioBoost(uint8_t boost);
void playbackSetBoost(uint8_t boost);
void playbackPollSettings();