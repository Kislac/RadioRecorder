#pragma once

#include <Arduino.h>
#include "driver/i2s_std.h"

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
enum class AdcProbeResult { ok, invalidPin, busy, unavailable, error };
constexpr size_t kAdcProbeSamples = 128;
AdcProbeResult recorderReadAdcSamples(uint8_t gpio, uint16_t *samples);
bool recorderTaskReady();
void recorderNotify();
bool recorderStart(bool sdAvailable, CaptureSource source = CaptureSource::adc, bool automatic = false);
uint64_t recorderElapsedMs();
bool recorderAutomaticFile();
bool recorderFinalized();
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
constexpr size_t kSpectrumPcmBytes = 1024;

uint32_t recorderStackFreeMinimum();
uint32_t recorderAdcOverflowCount();
bool recorderSpectrumSnapshot(CaptureSource source, uint8_t *samples);