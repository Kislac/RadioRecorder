#include <Arduino.h>
#include <SD_MMC.h>
#include <atomic>
#include "esp_adc/adc_continuous.h"
#include "live_audio.h"
#include "wav.h"
#include "recorder.h"

static constexpr size_t kDmaBufferSize = 4096;
static constexpr uint32_t kMaxWavBytes = 0xffffffffU - 44;
static adc_continuous_handle_t adcHandle = nullptr;
static adc_channel_t audioChannel;
static TaskHandle_t recorderTask = nullptr;
static std::atomic<bool> stopRequested{false};
static std::atomic<bool> adcOverflow{false};
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
static std::atomic<CaptureState> captureState{CaptureState::idle};
static std::atomic<uint32_t> recordingStartedAt{0};
static std::atomic<bool> recordingsPresent{false};
static bool adcReady = false;
static char recordingName[16] = {};
static char lastError[80] = {};
static uint8_t dmaBuffer[kDmaBufferSize];
static uint8_t pcmBuffer[kDmaBufferSize / 2];

bool recorderAdcReady() { return adcReady; }
bool recorderTaskReady() { return recorderTask != nullptr; }
void recorderNotify() { xTaskNotifyGive(recorderTask); }
CaptureState recorderState() { return captureState.load(); }
bool recorderStopping() { return stopRequested.load(); }
const char *recorderName() { return recordingName; }
const char *recorderError() { return lastError; }
bool recorderHasRecordings() { return recordingsPresent.load(); }
void recorderSetRecordingsPresent(bool present) { recordingsPresent.store(present); }
bool recorderLowpass() { return audioLowpassEnabled.load(); }
void recorderSetLowpass(bool enabled) { audioLowpassEnabled.store(enabled); }

SignalStatus recorderSignalStatus() {
  if (signalEnabled.load()) signalLastPoll.store(millis());
  return {signalEnabled.load(), signalActive.load(), signalSequence.load(), signalRms.load(),
          signalPeak.load(), signalBias.load(), signalFault.load()};
}

void recorderSetSignal(bool enabled) {
  if (enabled) {
    signalSequence.store(0);
    signalFault.store(0);
    signalLastPoll.store(millis());
  }
  signalEnabled.store(enabled);
  recorderNotify();
}

void recorderToggleSerialMeter() {
  bool enabled = !serialMeterEnabled.load();
  if (enabled && (!adcReady || recorderTask == nullptr)) {
    Serial.println("ADC meres nem elerheto");
    return;
  }
  serialMeterEnabled.store(enabled);
  if (enabled) {
    signalFault.store(0);
    recorderNotify();
  }
  Serial.println(enabled ? "Soros ADC meres bekapcsolva" : "Soros ADC meres kikapcsolva");
}

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
  return false;
}

struct SignalWindow {
  uint64_t squares = 0;
  uint32_t rawSum = 0;
  uint32_t samples = 0;
  uint16_t peak = 0;
};

bool signalRequested() {
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
    signalPeak.store(window.peak);
    signalBias.store(window.rawSum / window.samples);
    signalSequence.fetch_add(1);
    window = {};
  }
}


bool nextRecordingName() {
  for (uint32_t number = 1; number <= 99999; ++number) {
    snprintf(recordingName, sizeof(recordingName), "REC%05lu.wav", static_cast<unsigned long>(number));
    if (!SD_MMC.exists(String("/") + recordingName)) return true;
  }
  return false;
}

bool recorderStart(bool sdAvailable) {
  if (!sdAvailable || !adcReady || recorderTask == nullptr || captureState.load() != CaptureState::idle) return false;
  if (!nextRecordingName()) {
    snprintf(lastError, sizeof(lastError), "No free recording filename");
    return false;
  }
  lastError[0] = '\0';
  stopRequested.store(false);
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
    Serial.printf("ADC meresi hiba (%u)\n", fault);
    serialMeterEnabled.store(false);
    return;
  }
  uint32_t sequence = signalSequence.load();
  if (sequence == lastSequence) return;
  lastSequence = sequence;
  lastPrintAt = millis();
  char recordingLabel[64] = {};
  if (captureState.load() == CaptureState::recording) {
    uint32_t elapsedMs = lastPrintAt - recordingStartedAt.load();
    snprintf(recordingLabel, sizeof(recordingLabel), "REC %s | t=%lu.%03lu s | ", recordingName,
             static_cast<unsigned long>(elapsedMs / 1000), static_cast<unsigned long>(elapsedMs % 1000));
  }
  Serial.printf("%sGPIO%d RMS: %u | Csucs: %u | DC-kozep: %u (ADC)\n", recordingLabel, kAudioGpio,
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
  while ((signalRequested() || liveAudioClient() >= 0) && captureState.load() == CaptureState::idle) {
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

void recordLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (captureState.load() == CaptureState::idle && (signalRequested() || liveAudioClient() >= 0)) monitorIdle();
    if (captureState.load() != CaptureState::starting) continue;
    adcOverflow.store(false, std::memory_order_relaxed);
    String path = String("/") + recordingName;
    File file = SD_MMC.open(path, FILE_WRITE);
    uint32_t dataBytes = 0;
    bool started = false;
    bool valid = false;
    if (!file || !writeWavHeader(file, 0)) {
      snprintf(lastError, sizeof(lastError), "Cannot create WAV file");
    } else if (adc_continuous_flush_pool(adcHandle) != ESP_OK || adc_continuous_start(adcHandle) != ESP_OK) {
      snprintf(lastError, sizeof(lastError), "Cannot start ADC");
    } else {
      started = true;
      signalActive.store(true);
      recordingStartedAt.store(millis());
      captureState.store(CaptureState::recording);
      Serial.printf("Recording started: %s\n", recordingName);
      float dcLevel = 2048.0f;
      SignalWindow window;
      AudioLowpass lowpass;
      while (!stopRequested.load()) {
        bool publishSignal = signalRequested();
        if (adcOverflow.load(std::memory_order_relaxed)) {
          snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
          break;
        }
        uint32_t length = 0;
        esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
        if (result == ESP_ERR_TIMEOUT) continue;
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
        queueLiveAudio(pcmBuffer, pcmLength);
        if (dataBytes > kMaxWavBytes - pcmLength || file.write(pcmBuffer, pcmLength) != pcmLength) {
          snprintf(lastError, sizeof(lastError), "SD write failed or WAV size limit reached");
          break;
        }
        dataBytes += pcmLength;
      }
    }
    if (started) {
      adc_continuous_stop(adcHandle);
      signalActive.store(false);
    }
    if (adcOverflow.load(std::memory_order_relaxed) && !lastError[0]) {
      snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
    }
    if (file) {
      valid = writeWavHeader(file, dataBytes);
      file.close();
    }
    if (!valid) {
      SD_MMC.remove(path);
      if (!lastError[0]) snprintf(lastError, sizeof(lastError), "Cannot finish WAV file");
    }
    if (valid) {
      recordingsPresent.store(true);
      Serial.printf("Recording stopped: %s (%lu audio bytes)\n", recordingName, static_cast<unsigned long>(dataBytes));
    }
    else Serial.printf("Recording failed: %s\n", recordingName);
    if (lastError[0]) Serial.printf("Recording error: %s\n", lastError);
    captureState.store(CaptureState::idle);
    if (signalRequested() || liveAudioClient() >= 0) xTaskNotifyGive(recorderTask);
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

void initRecorderTask() {
  if (adcReady && xTaskCreatePinnedToCore(recordLoop, "recorder", 8192, nullptr, 2, &recorderTask, 1) != pdPASS) {
    Serial.println("Cannot create recorder task");
  }
}

