#pragma once

#include <Arduino.h>
#include "auto_record_rules.h"
#include "recorder.h"

struct AutomaticRecordStatus {
  bool armed = false;
  bool recording = false;
  uint16_t rms = 0;
  AutoRecordConfig config;
  uint64_t elapsedMs = 0;
  uint64_t lastElapsedMs = 0;
  String lastFile;
  String error;
  String blocked;
};

bool initAutomaticRecording();
void pollAutomaticRecording();
bool automaticRequestManual(CaptureSource source);
bool automaticStopCurrent();
bool automaticSetArmed(bool microphone, bool enabled);
bool automaticAnyArmed();
bool automaticMonitorRadio();
bool automaticMonitorMic();
void automaticPauseMonitoring();
void automaticObserve(bool microphone, uint16_t rms);
void automaticInputFault(bool microphone, const char *error);
AutomaticRecordStatus automaticStatus(bool microphone);
bool automaticSaveSettings(const AutoRecordConfig &radio, const AutoRecordConfig &mic,
                           const std::array<RecordingMode, 4> &priorities);
std::array<RecordingMode, 4> automaticPriorities();
RecordingMode automaticCurrentMode();
const char *recordingModeCode(RecordingMode mode);
const char *recordingModeLabel(RecordingMode mode);
String automaticRecordingJson();