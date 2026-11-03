#include <Arduino.h>
#include <SD_MMC.h>
#include <atomic>
#include <cstring>
#include "esp_adc/adc_continuous.h"
#include "driver/i2s_std.h"
#include "live_audio.h"
#include "app_control.h"
#include "playback.h"
#include "wav.h"
#include "recorder.h"
#include "watchdog.h"
#include "automatic_record.h"
#include "pcm_history.h"
#include "performance.h"

static constexpr size_t kDmaBufferSize = 4096;
static constexpr uint32_t kMaxWavBytes = 0xffffffffU - 44;
static adc_continuous_handle_t adcHandle = nullptr;
static i2s_chan_handle_t micHandle = nullptr;
static adc_channel_t audioChannel;
static TaskHandle_t recorderTask = nullptr;
static std::atomic<bool> stopRequested{false};
static std::atomic<bool> adcOverflow{false};
static std::atomic<uint32_t> adcOverflowCount{0};
static std::atomic<bool> adcProbeActive{false};
static std::atomic<bool> signalEnabled{false};
static std::atomic<bool> serialMeterEnabled{false};
static std::atomic<bool> audioLowpassEnabled{false};
static std::atomic<bool> signalActive{false};
static std::atomic<uint32_t> signalLastPoll{0};
static std::atomic<uint32_t> signalSequence{0};
static std::atomic<uint16_t> signalRms{0};
static std::atomic<uint16_t> signalPeak{0};
static std::atomic<uint16_t> signalBias{0};
static std::atomic<uint8_t> signalFault{0};
static std::atomic<bool> micSignalEnabled{false};
static std::atomic<bool> micSignalActive{false};
static std::atomic<uint32_t> micSignalLastPoll{0};
static std::atomic<uint32_t> micSignalSequence{0};
static std::atomic<uint16_t> micSignalRms{0};
static std::atomic<uint16_t> micSignalPeak{0};
static std::atomic<uint8_t> micSignalFault{0};
static std::atomic<CaptureState> captureState{CaptureState::idle};
static std::atomic<CaptureSource> captureSource{CaptureSource::adc};
static std::atomic<uint32_t> recordingStartedAt{0};
static std::atomic<uint32_t> recordedBytes{0};
static std::atomic<bool> automaticFile{false};
static std::atomic<bool> finalizedFile{false};
static std::atomic<bool> recordingsPresent{false};
static bool adcReady = false;
static bool micReady = false;
static char recordingName[64] = {};
static char lastError[80] = {};
static uint8_t dmaBuffer[kDmaBufferSize];
static uint8_t pcmBuffer[kDmaBufferSize / 2];
static uint8_t spectrumPcm[kSpectrumPcmBytes];
static portMUX_TYPE spectrumLock = portMUX_INITIALIZER_UNLOCKED;
static CaptureSource spectrumSource = CaptureSource::adc;
static bool spectrumAvailable = false;
static PcmHistory<kSampleRate> radioHistory, micHistory;
static float automaticDcLevel = 2048.0f;

bool recorderAdcReady() { return adcReady; }
bool recorderMicReady() { return micReady; }

static void publishSpectrum(CaptureSource source, const uint8_t *samples, size_t length) {
  if (length < kSpectrumPcmBytes) return;
  portENTER_CRITICAL(&spectrumLock);
  memcpy(spectrumPcm, samples + length - kSpectrumPcmBytes, kSpectrumPcmBytes);
  spectrumSource = source;
  spectrumAvailable = true;
  portEXIT_CRITICAL(&spectrumLock);
}

bool recorderSpectrumSnapshot(CaptureSource source, uint8_t *samples) {
  bool active = captureState.load() == CaptureState::recording && captureSource.load() == source;
  active = active || (source == CaptureSource::microphone ? micSignalActive.load() : signalActive.load());
  if (!active) return false;
  portENTER_CRITICAL(&spectrumLock);
  bool available = spectrumAvailable && spectrumSource == source;
  if (available) memcpy(samples, spectrumPcm, kSpectrumPcmBytes);
  portEXIT_CRITICAL(&spectrumLock);
  return available;
}

AdcProbeResult recorderReadAdcSamples(uint8_t gpio, uint16_t *samples) {
  if (automaticAnyArmed()) return AdcProbeResult::busy;
  if (gpio != 1 && gpio != 2 && gpio != 3 && gpio != 9) return AdcProbeResult::invalidPin;
  if (!adcReady || !adcHandle) return AdcProbeResult::unavailable;
  if (captureState.load() != CaptureState::idle || signalEnabled.load() || signalActive.load() ||
      serialMeterEnabled.load() || liveAudioClient() >= 0 || playbackGetState() != PlaybackState::idle ||
      adcProbeActive.exchange(true))
    return AdcProbeResult::busy;
  adc_unit_t unit;
  adc_channel_t channel;
  if (adc_continuous_io_to_channel(gpio, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
    adcProbeActive.store(false);
    return AdcProbeResult::invalidPin;
  }
  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = channel;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_BITWIDTH_12;
  adc_continuous_config_t config = {};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = kSampleRate;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  bool reconfigured = channel != audioChannel;
  esp_err_t result = reconfigured ? adc_continuous_config(adcHandle, &config) : ESP_OK;
  bool started = false;
  if (result == ESP_OK) result = adc_continuous_flush_pool(adcHandle);
  if (result == ESP_OK) {
    result = adc_continuous_start(adcHandle);
    started = result == ESP_OK;
  }
  if (started) {
    uint8_t frame[1024];
    uint32_t length = 0;
    result = adc_continuous_read(adcHandle, frame, sizeof(frame), &length, 100);
    size_t count = 0;
    if (result == ESP_OK && length % SOC_ADC_DIGI_RESULT_BYTES == 0) {
      for (uint32_t offset = 0; offset < length && count < kAdcProbeSamples; offset += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t sample;
        memcpy(&sample, frame + offset, sizeof(sample));
        if (sample.type2.unit == ADC_UNIT_1 && sample.type2.channel == channel)
          samples[count++] = sample.type2.data;
      }
    }
    if (count != kAdcProbeSamples) result = ESP_FAIL;
    esp_err_t stopped = adc_continuous_stop(adcHandle);
    if (stopped != ESP_OK) result = stopped;
  }
  adc_digi_pattern_config_t originalPattern = pattern;
  originalPattern.channel = audioChannel;
  adc_continuous_config_t restore = config;
  restore.adc_pattern = &originalPattern;
  if (reconfigured && adc_continuous_config(adcHandle, &restore) != ESP_OK) {
    adcReady = false;
    result = ESP_FAIL;
  }
  adcProbeActive.store(false);
  return result == ESP_OK ? AdcProbeResult::ok : AdcProbeResult::error;
}

bool recorderTaskReady() { return recorderTask != nullptr; }
uint32_t recorderStackFreeMinimum() { return recorderTask ? uxTaskGetStackHighWaterMark(recorderTask) : 0; }
uint32_t recorderAdcOverflowCount() { return adcOverflowCount.load(); }
uint64_t recorderElapsedMs() { return static_cast<uint64_t>(recordedBytes.load()) * 1000ULL / (kSampleRate * 2ULL); }
bool recorderAutomaticFile() { return automaticFile.load(); }
bool recorderFinalized() { return finalizedFile.load(); }
void recorderNotify() { if (recorderTask) xTaskNotifyGive(recorderTask); }
CaptureState recorderState() { return captureState.load(); }
CaptureSource recorderSource() { return captureSource.load(); }
bool recorderStopping() { return captureState.load() != CaptureState::idle && stopRequested.load(); }
const char *recorderName() { return recordingName; }
const char *recorderError() { return lastError; }
bool recorderHasRecordings() { return recordingsPresent.load(); }
void recorderSetRecordingsPresent(bool present) { recordingsPresent.store(present); }

static bool isRecordingName(const char *filename) {
  if (!filename) return false;
  if (strlen(filename) == 12 && strncmp(filename, "REC", 3) == 0 && strcmp(filename + 8, ".wav") == 0) {
    for (uint8_t index = 3; index < 8; ++index) if (!isDigit(filename[index])) return false;
    return true;
  }
  if (strlen(filename) < 35 || filename[0] != '/' || filename[5] != '-' || filename[8] != '-' ||
      filename[11] != '/' || strcmp(filename + strlen(filename) - 4, ".wav") != 0) return false;
  for (int index : {1, 2, 3, 4, 6, 7, 9, 10}) if (!isDigit(filename[index])) return false;
  const char *name = filename + 12;
  if (strncmp(name, "RadioMan", 8) == 0 || strncmp(name, "RadioAut", 8) == 0) name += 8;
  else if (strncmp(name, "MicMan", 6) == 0 || strncmp(name, "MicAut", 6) == 0) name += 6;
  else return false;
  if (strlen(name) != 17 && strlen(name) != 20) return false;
  for (int index : {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12}) if (!isDigit(name[index])) return false;
  if (name[6] != '_' || strncmp(filename + 3, name + 0, 2) != 0 ||
      strncmp(filename + 6, name + 2, 2) != 0 || strncmp(filename + 9, name + 4, 2) != 0) return false;
  return strlen(name) == 17 || (name[13] == '_' && isDigit(name[14]) && isDigit(name[15]));
}

static bool hasAnyRecording() {
  File root = SD_MMC.open("/");
  if (!root) return false;
  bool found = false;
  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    if (entry.isDirectory()) {
      String folder = name.startsWith("/") ? name : "/" + name;
      File directory = SD_MMC.open(folder);
      File child;
      while (directory && (child = directory.openNextFile())) {
        String childName = child.name();
        String path = folder + "/" + childName.substring(childName.lastIndexOf('/') + 1);
        found = !child.isDirectory() && child.size() >= 44 && isRecordingName(path.c_str());
        child.close();
        if (found) break;
      }
      directory.close();
    } else found = entry.size() >= 44 && isRecordingName(name.substring(name.lastIndexOf('/') + 1).c_str());
    entry.close();
    if (found) break;
  }
  root.close();
  return found;
}

bool recorderDeleteRecording(const char *filename) {
  if (captureState.load() != CaptureState::idle || !isRecordingName(filename)) {
    return false;
  }
  String path = filename[0] == '/' ? String(filename) : String("/") + filename;
  if (!SD_MMC.exists(path) || !SD_MMC.remove(path) || SD_MMC.exists(path)) return false;
  recordingsPresent.store(hasAnyRecording());
  if (path == recordingName) recordingName[0] = '\0';
  return true;
}

bool recorderLowpass() { return audioLowpassEnabled.load(); }
void recorderSetLowpass(bool enabled) { audioLowpassEnabled.store(enabled); }

SignalStatus recorderSignalStatus() {
  if (signalEnabled.load()) signalLastPoll.store(millis());
  return {signalEnabled.load(), signalActive.load(), signalSequence.load(), signalRms.load(),
          signalPeak.load(), signalBias.load(), signalFault.load()};
}

void recorderSetSignal(bool enabled) {
  if (enabled && micSignalEnabled.load()) return;
  if (enabled) {
    signalSequence.store(0);
    signalFault.store(0);
    signalLastPoll.store(millis());
  }
  signalEnabled.store(enabled);
  recorderNotify();
}

SignalStatus recorderMicSignalStatus() {
  if (micSignalEnabled.load()) micSignalLastPoll.store(millis());
  return {micSignalEnabled.load(), micSignalActive.load(), micSignalSequence.load(), micSignalRms.load(),
          micSignalPeak.load(), 0, micSignalFault.load()};
}

bool recorderSetMicSignal(bool enabled) {
  if (enabled && (!micReady || !recorderTask || signalEnabled.load() || serialMeterEnabled.load() ||
                  liveAudioClient() >= 0 || (captureState.load() != CaptureState::idle &&
                  captureSource.load() != CaptureSource::microphone))) return false;
  if (enabled) {
    micSignalSequence.store(0);
    micSignalFault.store(0);
    micSignalLastPoll.store(millis());
  } else micSignalActive.store(false);
  micSignalEnabled.store(enabled);
  recorderNotify();
  return true;
}

bool recorderSerialMeterEnabled() { return serialMeterEnabled.load(); }

bool recorderSetSerialMeter(bool enabled) {
  if (enabled && (!adcReady || recorderTask == nullptr || micSignalEnabled.load())) {
    Serial.println("ADC measurement unavailable");
    return false;
  }
  serialMeterEnabled.store(enabled);
  if (enabled) {
    signalFault.store(0);
    recorderNotify();
  }
  Serial.println(enabled ? "Serial ADC measurement enabled" : "Serial ADC measurement disabled");
  return true;
}

void recorderToggleSerialMeter() { recorderSetSerialMeter(!recorderSerialMeterEnabled()); }

struct AudioLowpass {
  float input1 = 0;
  float input2 = 0;
  float output1 = 0;
  float output2 = 0;

  float process(float input) {
    float output = 0.1370154f * input + 0.2740308f * input1 + 0.1370154f * input2
                   + 0.7155086f * output1 - 0.2635703f * output2;
    input2 = input1;
    input1 = input;
    output2 = output1;
    output1 = output;
    return output;
  }
};

int16_t encodePcm(float centered, AudioLowpass &lowpass) {
  float filtered = lowpass.process(centered);
  float value = audioLowpassEnabled.load() ? filtered : centered;
  return static_cast<int16_t>(constrain(static_cast<int32_t>(value * 16), -32768, 32767));
}


bool adcPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  adcOverflow.store(true, std::memory_order_relaxed);
  if (performanceEnabled()) adcOverflowCount.fetch_add(1, std::memory_order_relaxed);
  return false;
}

struct SignalWindow {
  uint64_t squares = 0;
  uint32_t rawSum = 0;
  uint32_t samples = 0;
  uint16_t peak = 0;
};

bool micSignalRequested() {
  if (automaticMonitorMic()) return true;
  if (!micSignalEnabled.load()) return false;
  if (static_cast<uint32_t>(millis() - micSignalLastPoll.load()) <= 4000) return true;
  micSignalEnabled.store(false);
  micSignalActive.store(false);
  return false;
}

void addMicSignalSample(SignalWindow &window, int16_t sample) {
  uint16_t amplitude = abs(static_cast<int32_t>(sample));
  window.squares += static_cast<uint32_t>(amplitude) * amplitude;
  if (amplitude > window.peak) window.peak = amplitude;
  if (++window.samples >= kSampleRate / 10) {
    micSignalRms.store(static_cast<uint16_t>(sqrtf(static_cast<float>(window.squares) / window.samples)));
    automaticObserve(true, micSignalRms.load());
    micSignalPeak.store(window.peak);
    micSignalSequence.fetch_add(1);
    window = {};
  }
}

esp_err_t readMicFrame(SignalWindow &window, size_t &pcmLength, uint32_t timeout = 100) {
  size_t length = 0;
  esp_err_t result = i2s_channel_read(micHandle, dmaBuffer, sizeof(dmaBuffer), &length, timeout);
  if (result == ESP_ERR_TIMEOUT && timeout == 0 && length) result = ESP_OK;
  if (result != ESP_OK) return result;
  if (!length || length % (2 * sizeof(int32_t)) != 0) return ESP_FAIL;
  bool publishSignal = micSignalRequested();
  pcmLength = 0;
  for (size_t offset = 0; offset < length; offset += 2 * sizeof(int32_t)) {
    int32_t sample;
    memcpy(&sample, dmaBuffer + offset, sizeof(sample));
    int16_t pcm = static_cast<int16_t>(sample >> 16);
    put16(pcmBuffer + pcmLength, static_cast<uint16_t>(pcm));
    pcmLength += 2;
    if (publishSignal) addMicSignalSample(window, pcm);
  }
  if (automaticMonitorMic()) micHistory.push(pcmBuffer, pcmLength);
  publishSpectrum(CaptureSource::microphone, pcmBuffer, pcmLength);
  return ESP_OK;
}

bool signalRequested() {
  if (automaticMonitorRadio()) return true;
  if (serialMeterEnabled.load()) return true;
  if (!signalEnabled.load()) return false;
  if (static_cast<uint32_t>(millis() - signalLastPoll.load()) <= 4000) return true;
  signalEnabled.store(false);
  return false;
}

void addSignalSample(SignalWindow &window, uint16_t raw, int32_t centered) {
  uint16_t amplitude = abs(centered);
  window.squares += static_cast<uint64_t>(amplitude) * amplitude;
  window.rawSum += raw;
  if (amplitude > window.peak) window.peak = amplitude;
  if (++window.samples >= kSampleRate / 10) {
    signalRms.store(static_cast<uint16_t>(sqrtf(static_cast<float>(window.squares) / window.samples)));
    automaticObserve(false, signalRms.load());
    signalPeak.store(window.peak);
    signalBias.store(window.rawSum / window.samples);
    signalSequence.fetch_add(1);
    window = {};
  }
}


bool nextRecordingName(CaptureSource source) {
  tm local;
  if (!appClockLocal(local)) return false;
  char directory[12];
  strftime(directory, sizeof(directory), "%Y-%m-%d", &local);
  String folder = String("/") + directory;
  if (!SD_MMC.exists(folder) && !SD_MMC.mkdir(folder)) return false;
  char stamp[16];
  strftime(stamp, sizeof(stamp), "%y%m%d_%H%M%S", &local);
  const char *prefix = source == CaptureSource::microphone ? (automaticFile.load() ? "MicAut" : "MicMan") : (automaticFile.load() ? "RadioAut" : "RadioMan");
  for (unsigned int number = 0; number <= 99; ++number) {
    snprintf(recordingName, sizeof(recordingName), number == 0 ? "/%s/%s%s.wav" : "/%s/%s%s_%02u.wav",
             directory, prefix, stamp, number);
    if (!SD_MMC.exists(recordingName)) return true;
  }
  return false;
}

bool recorderStart(bool sdAvailable, CaptureSource source, bool automatic) {
  if (!sdAvailable || !(source == CaptureSource::adc ? adcReady : micReady) || recorderTask == nullptr ||
  captureState.load() != CaptureState::idle || adcProbeActive.load()) return false;
  automaticFile.store(automatic);
  if (!nextRecordingName(source)) {
    snprintf(lastError, sizeof(lastError), "No free recording filename");
    return false;
  }
  lastError[0] = '\0';
  recordedBytes.store(0);
  finalizedFile.store(false);
  stopRequested.store(false);
  captureSource.store(source);
  captureState.store(CaptureState::starting);
  xTaskNotifyGive(recorderTask);
  return true;
}

bool stopRecording() {
  if (captureState.load() == CaptureState::idle) return false;
  stopRequested.store(true);
  return true;
}


void pollSerialMeter() {
  static bool wasEnabled = false;
  static uint32_t lastSequence = 0;
  static uint32_t lastPrintAt = 0;
  if (!serialMeterEnabled.load()) {
    wasEnabled = false;
    return;
  }
  if (!wasEnabled) {
    lastSequence = signalSequence.load();
    lastPrintAt = millis() - 500;
    wasEnabled = true;
  }
  if (millis() - lastPrintAt < 500) return;
  uint8_t fault = signalFault.load();
  if (fault) {
    Serial.printf("ADC measurement error (%u)\n", fault);
    serialMeterEnabled.store(false);
    return;
  }
  uint32_t sequence = signalSequence.load();
  if (sequence == lastSequence) return;
  lastSequence = sequence;
  lastPrintAt = millis();
  char recordingLabel[96] = {};
  if (captureState.load() == CaptureState::recording) {
    uint32_t elapsedMs = lastPrintAt - recordingStartedAt.load();
    snprintf(recordingLabel, sizeof(recordingLabel), "REC %s | t=%lu.%03lu s | ", recordingName,
             static_cast<unsigned long>(elapsedMs / 1000), static_cast<unsigned long>(elapsedMs % 1000));
  }
  Serial.printf("%sGPIO%d RMS: %u | Peak: %u | DC-offset: %u (ADC)\n", recordingLabel, kAudioGpio,
                signalRms.load(), signalPeak.load(), signalBias.load());
}


void monitorIdle() {
  adcOverflow.store(false, std::memory_order_relaxed);
  if (adc_continuous_flush_pool(adcHandle) != ESP_OK || adc_continuous_start(adcHandle) != ESP_OK) {
    signalFault.store(1);
    signalEnabled.store(false);
    faultLiveAudio();
    return;
  }
  signalActive.store(true);
  SignalWindow window;
  AudioLowpass lowpass;
  float dcLevel = 2048.0f;
  while (!automaticAnyArmed() && !automaticMonitorRadio() && !automaticMonitorMic() && (signalRequested() || liveAudioClient() >= 0) && captureState.load() == CaptureState::idle) {
    watchdogResetCurrentTask();
    if (adcOverflow.load(std::memory_order_relaxed)) {
      signalFault.store(2);
      signalEnabled.store(false);
      faultLiveAudio();
      break;
    }
    uint32_t length = 0;
    esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES != 0) {
      signalFault.store(3);
      signalEnabled.store(false);
      faultLiveAudio();
      break;
    }
    uint32_t validSamples = 0;
    size_t pcmLength = 0;
    bool publishSignal = signalRequested();
    for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
      adc_digi_output_data_t sample;
      memcpy(&sample, dmaBuffer + offset, sizeof(sample));
      if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != audioChannel) continue;
      ++validSamples;
      dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
      float centered = static_cast<float>(sample.type2.data) - dcLevel;
      if (publishSignal) addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
      if (liveAudioClient() >= 0) {
        put16(pcmBuffer + pcmLength, static_cast<uint16_t>(encodePcm(centered, lowpass)));
        pcmLength += 2;
      }
    }
    if (!validSamples) {
      signalFault.store(3);
      signalEnabled.store(false);
      faultLiveAudio();
      break;
    }
    if (pcmLength) queueLiveAudio(pcmBuffer, pcmLength);
  }
  adc_continuous_stop(adcHandle);
  signalActive.store(false);
}

void monitorMicIdle() {
  if (i2s_channel_enable(micHandle) != ESP_OK) {
    micSignalFault.store(1);
    micSignalEnabled.store(false);
    return;
  }
  micSignalActive.store(true);
  SignalWindow window;
  while (!automaticAnyArmed() && !automaticMonitorRadio() && !automaticMonitorMic() && micSignalRequested() && captureState.load() == CaptureState::idle) {
    watchdogResetCurrentTask();
    size_t pcmLength = 0;
    esp_err_t result = readMicFrame(window, pcmLength);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK) {
      micSignalFault.store(2);
      micSignalEnabled.store(false);
      break;
    }
  }
  i2s_channel_disable(micHandle);
  micSignalActive.store(false);
}

void monitorAutomaticAdc(SignalWindow &window, AudioLowpass &lowpass) {
  if (adcOverflow.load()) { automaticInputFault(false, "ADC buffer overflow"); return; }
  uint32_t length = 0;
  esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 0);
  if (result == ESP_ERR_TIMEOUT) return;
  if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES) { automaticInputFault(false, "ADC read failed"); return; }
  size_t pcmLength = 0;
  for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
    adc_digi_output_data_t sample; memcpy(&sample, dmaBuffer + offset, sizeof(sample));
    if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != audioChannel) continue;
    automaticDcLevel += (static_cast<float>(sample.type2.data) - automaticDcLevel) / 1024.0f;
    float centered = static_cast<float>(sample.type2.data) - automaticDcLevel;
    addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
    put16(pcmBuffer + pcmLength, static_cast<uint16_t>(encodePcm(centered, lowpass))); pcmLength += 2;
  }
  if (automaticMonitorRadio()) radioHistory.push(pcmBuffer, pcmLength);
}

void automaticSensor(bool microphone, bool enabled, bool &active) {
  if (enabled == active) return;
  if (enabled) {
    esp_err_t result;
    if (microphone) result = i2s_channel_enable(micHandle);
    else { adcOverflow.store(false); adc_continuous_flush_pool(adcHandle); result = adc_continuous_start(adcHandle); }
    if (result != ESP_OK) { automaticInputFault(microphone, "Cannot start audio monitoring"); return; }
    automaticInputStarted(microphone);
    active = true;
  } else {
    if (microphone) i2s_channel_disable(micHandle); else adc_continuous_stop(adcHandle);
    active = false;
  }
  if (microphone) micSignalActive.store(active); else signalActive.store(active);
}

void monitorAutomaticIdle() {
  bool adcActive = false, micActive = false;
  SignalWindow adcWindow, micWindow; AudioLowpass lowpass;
  while (captureState.load() == CaptureState::idle && (automaticMonitorRadio() || automaticMonitorMic())) {
    automaticSensor(false, automaticMonitorRadio() || signalRequested(), adcActive);
    automaticSensor(true, automaticMonitorMic() || micSignalRequested(), micActive);
    if (adcActive) monitorAutomaticAdc(adcWindow, lowpass);
    if (micActive) {
      size_t length = 0; esp_err_t result = readMicFrame(micWindow, length, 0);
      if (result != ESP_OK && result != ESP_ERR_TIMEOUT) automaticInputFault(true, "Microphone read failed");
    }
    watchdogResetCurrentTask(); vTaskDelay(1);
  }
  automaticSensor(false, false, adcActive); automaticSensor(true, false, micActive);
}

void monitorOtherAutomatic(bool microphoneRecording, bool &otherActive, SignalWindow &window, AudioLowpass &lowpass) {
  bool otherMic = !microphoneRecording;
  bool needed = otherMic ? automaticMonitorMic() || (automaticAnyArmed() && micSignalRequested()) :
                          automaticMonitorRadio() || (automaticAnyArmed() && signalRequested());
  automaticSensor(otherMic, needed, otherActive);
  if (!otherActive) return;
  if (otherMic) {
    for (uint8_t frames = 0; frames < 8; ++frames) {
      size_t length = 0; esp_err_t result = readMicFrame(window, length, 0);
      if (result == ESP_ERR_TIMEOUT) break;
      if (result != ESP_OK) { automaticInputFault(true, "Microphone read failed"); break; }
    }
  } else monitorAutomaticAdc(window, lowpass);
}

void recordLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (captureState.load() == CaptureState::idle && (automaticMonitorRadio() || automaticMonitorMic())) {
      bool monitorWatchdog = watchdogSubscribeCurrentTask();
      monitorAutomaticIdle(); watchdogUnsubscribeCurrentTask(monitorWatchdog);
    }
    if (captureState.load() == CaptureState::idle && micSignalRequested()) {
      bool monitorWatchdog = watchdogSubscribeCurrentTask();
      monitorMicIdle();
      watchdogUnsubscribeCurrentTask(monitorWatchdog);
    }
    if (!adcProbeActive.load() && captureState.load() == CaptureState::idle &&
      (signalRequested() || liveAudioClient() >= 0)) {
      bool monitorWatchdog = watchdogSubscribeCurrentTask();
      monitorIdle();
      watchdogUnsubscribeCurrentTask(monitorWatchdog);
    }
    if (captureState.load() != CaptureState::starting) continue;
    bool recordingWatchdog = watchdogSubscribeCurrentTask();
    CaptureSource source = captureSource.load();
    bool microphone = source == CaptureSource::microphone;
    adcOverflow.store(false, std::memory_order_relaxed);
    String path = recordingName;
    File file = SD_MMC.open(path, FILE_WRITE);
    uint32_t dataBytes = 0;
    bool otherActive = false;
    bool started = false;
    bool valid = false;
    if (!file || !writeWavHeader(file, 0)) {
      snprintf(lastError, sizeof(lastError), "Cannot create WAV file");
    } else if (automaticFile.load() && !(microphone ? micHistory : radioHistory).write(file, dataBytes)) {
      snprintf(lastError, sizeof(lastError), "Cannot write recording prebuffer");
    } else if (microphone ? i2s_channel_enable(micHandle) != ESP_OK :
               adc_continuous_flush_pool(adcHandle) != ESP_OK || adc_continuous_start(adcHandle) != ESP_OK) {
      snprintf(lastError, sizeof(lastError), microphone ? "Cannot start microphone" : "Cannot start ADC");
    } else {
      started = true;
      automaticInputStarted(microphone);
      if (!microphone) signalActive.store(true);
      recordedBytes.store(dataBytes);
      recordingStartedAt.store(millis());
      captureState.store(CaptureState::recording);
      Serial.printf("Recording started: %s (%s)\n", recordingName, microphone ? "microphone" : "ADC");
      playbackRecordCue(automaticFile.load(), microphone, true);
      float dcLevel = automaticFile.load() ? automaticDcLevel : 2048.0f;
      SignalWindow window;
      SignalWindow micWindow;
      SignalWindow otherWindow; AudioLowpass otherLowpass;
      AudioLowpass lowpass;
      while (!stopRequested.load()) {
        if (recordingWatchdog) watchdogResetCurrentTask();
        if (microphone) {
          micSignalActive.store(micSignalRequested());
          size_t pcmLength = 0;
          esp_err_t result = readMicFrame(micWindow, pcmLength);
          if (result == ESP_ERR_TIMEOUT) { monitorOtherAutomatic(microphone, otherActive, otherWindow, otherLowpass); continue; }
          if (result != ESP_OK) {
            snprintf(lastError, sizeof(lastError), "Microphone read failed");
            break;
          }
          if (dataBytes > kMaxWavBytes - pcmLength || file.write(pcmBuffer, pcmLength) != pcmLength) {
            snprintf(lastError, sizeof(lastError), "SD write failed or WAV size limit reached");
            break;
          }
          dataBytes += pcmLength;
          recordedBytes.store(dataBytes);
          monitorOtherAutomatic(microphone, otherActive, otherWindow, otherLowpass);
          continue;
        }
        bool publishSignal = signalRequested();
        if (adcOverflow.load(std::memory_order_relaxed)) {
          snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
          break;
        }
        uint32_t length = 0;
        esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
        if (result == ESP_ERR_TIMEOUT) { monitorOtherAutomatic(microphone, otherActive, otherWindow, otherLowpass); continue; }
        if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES != 0) {
          snprintf(lastError, sizeof(lastError), "ADC read failed or samples lost");
          break;
        }
        size_t pcmLength = 0;
        for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
          adc_digi_output_data_t sample;
          memcpy(&sample, dmaBuffer + offset, sizeof(sample));
          if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != audioChannel) continue;
          dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
          float centered = static_cast<float>(sample.type2.data) - dcLevel;
          if (publishSignal) addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
          put16(pcmBuffer + pcmLength, static_cast<uint16_t>(encodePcm(centered, lowpass)));
          pcmLength += 2;
        }
        if (pcmLength == 0) {
          snprintf(lastError, sizeof(lastError), "ADC returned no valid samples");
          break;
        }
        if (automaticMonitorRadio()) automaticDcLevel = dcLevel;
        queueLiveAudio(pcmBuffer, pcmLength);
        publishSpectrum(CaptureSource::adc, pcmBuffer, pcmLength);
        if (dataBytes > kMaxWavBytes - pcmLength || file.write(pcmBuffer, pcmLength) != pcmLength) {
          snprintf(lastError, sizeof(lastError), "SD write failed or WAV size limit reached");
          break;
        }
        dataBytes += pcmLength;
        recordedBytes.store(dataBytes);
        if (automaticMonitorRadio()) radioHistory.push(pcmBuffer, pcmLength);
        monitorOtherAutomatic(microphone, otherActive, otherWindow, otherLowpass);
      }
    }
    if (started) {
      automaticSensor(!microphone, false, otherActive);
      if (microphone) { i2s_channel_disable(micHandle); micSignalActive.store(false); }
      else { adc_continuous_stop(adcHandle); signalActive.store(false); }
    }
    if (adcOverflow.load(std::memory_order_relaxed) && !lastError[0]) {
      snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
    }
    if (file) {
      valid = started && writeWavHeader(file, dataBytes);
      file.close();
    }
    if (!valid) {
      SD_MMC.remove(path);
      if (!lastError[0]) snprintf(lastError, sizeof(lastError), "Cannot finish WAV file");
    }
    if (valid) {
      finalizedFile.store(true);
      recordingsPresent.store(true);
      Serial.printf("Recording stopped: %s (%lu audio bytes)\n", recordingName, static_cast<unsigned long>(dataBytes));
    }
    else Serial.printf("Recording failed: %s\n", recordingName);
    if (lastError[0]) Serial.printf("Recording error: %s\n", lastError);
    captureState.store(CaptureState::idle);
    if (valid) playbackRecordCue(automaticFile.load(), microphone, false);
    watchdogUnsubscribeCurrentTask(recordingWatchdog);
    if (signalRequested() || micSignalRequested() || liveAudioClient() >= 0) xTaskNotifyGive(recorderTask);
  }
}

static bool initAdc() {
  adc_unit_t unit;
  if (adc_continuous_io_to_channel(kAudioGpio, &unit, &audioChannel) != ESP_OK || unit != ADC_UNIT_1) return false;
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = 32768;
  handleConfig.conv_frame_size = 1024;
  if (adc_continuous_new_handle(&handleConfig, &adcHandle) != ESP_OK) return false;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = audioChannel;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t config = {};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = kSampleRate;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  adc_continuous_evt_cbs_t callbacks = {};
  callbacks.on_pool_ovf = adcPoolOverflow;
  if (adc_continuous_config(adcHandle, &config) == ESP_OK &&
      adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr) == ESP_OK) return true;
  adc_continuous_deinit(adcHandle);
  adcHandle = nullptr;
  return false;
}

bool initRecorderAdc() {
  adcReady = initAdc();
  return adcReady;
}

bool initRecorderMic() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel, nullptr, &micHandle) != ESP_OK) return false;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = GPIO_NUM_6;
  config.gpio_cfg.ws = GPIO_NUM_7;
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = GPIO_NUM_8;
  micReady = i2s_channel_init_std_mode(micHandle, &config) == ESP_OK &&
             gpio_set_pull_mode(GPIO_NUM_8, GPIO_PULLDOWN_ONLY) == ESP_OK;
  if (!micReady) {
    i2s_del_channel(micHandle);
    micHandle = nullptr;
    return false;
  }
  return micReady;
}

void initRecorderTask() {
  if ((adcReady || micReady) && xTaskCreatePinnedToCore(recordLoop, "recorder", 8192, nullptr, 2, &recorderTask, 1) != pdPASS) {
    Serial.println("Cannot create recorder task");
  }
}

