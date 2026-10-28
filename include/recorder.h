#pragma once

#include <Arduino.h>

constexpr int kAudioGpio = 1;

enum class CaptureState { idle, starting, recording };
enum class CaptureSource { adc, microphone };

struct SignalStatus {
  bool enabled;
  bool active;
  uint32_t sequence;
  uint16_t rms;
  uint16_t peak;
  uint16_t bias;
  uint8_t fault;
};

bool initRecorderAdc();
bool initRecorderMic();
void initRecorderTask();
bool recorderAdcReady();
bool recorderMicReady();
bool recorderTaskReady();
void recorderNotify();
bool recorderStart(bool sdAvailable, CaptureSource source = CaptureSource::adc);
bool stopRecording();
CaptureState recorderState();
CaptureSource recorderSource();
bool recorderStopping();
const char *recorderName();
const char *recorderError();
bool recorderHasRecordings();
void recorderSetRecordingsPresent(bool present);
bool recorderDeleteRecording(const char *filename);
bool recorderLowpass();
void recorderSetLowpass(bool enabled);
void recorderToggleSerialMeter();
bool recorderSetSerialMeter(bool enabled);
bool recorderSerialMeterEnabled();
void pollSerialMeter();
SignalStatus recorderSignalStatus();
void recorderSetSignal(bool enabled);
SignalStatus recorderMicSignalStatus();
bool recorderSetMicSignal(bool enabled);