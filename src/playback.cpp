#include "playback.h"

#include <SD_MMC.h>
#include <Preferences.h>
#include <atomic>
#include <cmath>
#include <esp_err.h>
#include "driver/i2s_std.h"
#include "recorder.h"
#include "wav.h"
#include "watchdog.h"

static constexpr gpio_num_t kSpeakerBclkGpio = GPIO_NUM_42;
static constexpr gpio_num_t kSpeakerLrcGpio = GPIO_NUM_41;
static constexpr gpio_num_t kSpeakerDataGpio = GPIO_NUM_11;
static constexpr gpio_num_t kDacBclkGpio = GPIO_NUM_18;
static constexpr gpio_num_t kDacLrcGpio = GPIO_NUM_13;
static constexpr gpio_num_t kDacDataGpio = GPIO_NUM_15;
static i2s_chan_handle_t speakerHandle = nullptr;
static DacToneSettings toneSettings = {};
static std::atomic<bool> toneRequested{false};
static std::atomic<PlaybackState> playbackState{PlaybackState::idle};
static std::atomic<bool> playbackStopRequested{false};
static std::atomic<bool> cuePlaying{false};
static std::atomic<uint8_t> requestedCue{0};
static std::atomic<uint16_t> recordCueSettings{0x55AA};
static std::atomic<uint8_t> speakerVolume{25};
static std::atomic<uint8_t> speakerBoost{1};
static std::atomic<uint8_t> radioVolume{5};
static std::atomic<uint8_t> radioBoost{1};
static std::atomic<uint32_t> playbackBytesPlayed{0};
static std::atomic<uint32_t> playbackBytesTotal{0};
static std::atomic<bool> playbackPaused{false};
static std::atomic<uint32_t> playbackSeekOffset{UINT32_MAX};
static constexpr char kSpeakerSettingsNamespace[] = "radio-recorder";
static constexpr char kSpeakerVolumeKey[] = "spkVol";
static constexpr char kSpeakerBoostKey[] = "spkBoost";
static constexpr char kRadioVolumeKey[] = "radioVol";
static constexpr char kRadioBoostKey[] = "radioGain";
static constexpr char kRecordCueKey[] = "recCues";
static constexpr uint32_t kSpeakerSaveDelayMs = 1000;
static uint8_t savedSpeakerVolume = 25;
static uint8_t savedSpeakerBoost = 1;
static uint8_t savedRadioVolume = 5;
static uint8_t savedRadioBoost = 1;
static uint32_t speakerSettingsChangedAt = 0;
static bool speakerSettingsDirty = false;
static TaskHandle_t playbackTask = nullptr;
uint32_t playbackStackFreeMinimum() { return playbackTask ? uxTaskGetStackHighWaterMark(playbackTask) : 0; }
static char playbackPath[221] = {};
static char playbackError[80] = {};
static bool speakerReady = false;
static PlaybackTarget playbackTarget = PlaybackTarget::speaker;

struct CueNote {
  uint16_t frequency;
  uint16_t durationMs;
};

static constexpr CueNote kStartupCue[] = {{523, 110}, {659, 110}, {784, 140}, {1047, 240}};
static constexpr CueNote kRecordStartCue[] = {{659, 110}, {880, 150}, {1175, 210}};
static constexpr CueNote kRecordStopCue[] = {{1047, 110}, {784, 110}, {587, 230}};
static constexpr CueNote kAutoStartCue[] = {{880, 65}, {1175, 65}};
static constexpr CueNote kAutoStopCue[] = {{784, 65}, {587, 65}};

static void writeCueNotes(const CueNote *notes, size_t count) {
  if (!speakerReady || !speakerHandle || i2s_channel_enable(speakerHandle) != ESP_OK) return;
  constexpr size_t kChunkFrames = 128;
  int16_t output[kChunkFrames * 2];
  const float volume = fminf(0.35f, speakerVolume.load() * speakerBoost.load() / 100.0f);
  bool failed = false;
  for (size_t noteIndex = 0; noteIndex < count && !failed; ++noteIndex) {
    const CueNote &note = notes[noteIndex];
    const uint32_t frameCount = kSampleRate * note.durationMs / 1000;
    uint32_t frame = 0;
    while (frame < frameCount) {
      size_t chunkFrames = fminf(kChunkFrames, frameCount - frame);
      for (size_t index = 0; index < chunkFrames; ++index) {
        uint32_t currentFrame = frame + index;
        float envelope = fminf(1.0f, fminf(currentFrame / 180.0f, (frameCount - currentFrame) / 360.0f));
        float phase = 2.0f * PI * note.frequency * currentFrame / kSampleRate;
        int16_t sample = note.frequency == 0 ? 0 : static_cast<int16_t>(sinf(phase) * 10000.0f * volume * envelope);
        output[index * 2] = sample;
        output[index * 2 + 1] = sample;
      }
      size_t written = 0;
      size_t bytes = chunkFrames * 2 * sizeof(int16_t);
      if (i2s_channel_write(speakerHandle, output, bytes, &written, 1000) != ESP_OK || written != bytes) {
        failed = true;
        break;
      }
      watchdogResetCurrentTask();
      frame += chunkFrames;
    }
  }
  i2s_channel_disable(speakerHandle);
}

static void playCue(PlaybackCue cue) {
  switch (cue) {
    case PlaybackCue::startup: writeCueNotes(kStartupCue, sizeof(kStartupCue) / sizeof(kStartupCue[0])); break;
    case PlaybackCue::recordingStarted: writeCueNotes(kRecordStartCue, sizeof(kRecordStartCue) / sizeof(kRecordStartCue[0])); break;
    case PlaybackCue::recordingStopped: writeCueNotes(kRecordStopCue, sizeof(kRecordStopCue) / sizeof(kRecordStopCue[0])); break;
    case PlaybackCue::autoStarted: writeCueNotes(kAutoStartCue, sizeof(kAutoStartCue) / sizeof(kAutoStartCue[0])); break;
    case PlaybackCue::autoStopped: writeCueNotes(kAutoStopCue, sizeof(kAutoStopCue) / sizeof(kAutoStopCue[0])); break;
  }
}

PlaybackState playbackGetState() {
  return cuePlaying.load() ? PlaybackState::playing : playbackState.load();
}

PlaybackStatus playbackStatus() {
  PlaybackState state = playbackGetState();
  return {state, speakerReady && playbackTask != nullptr, speakerReady && playbackTask != nullptr,
      cuePlaying.load() ? PlaybackTarget::speaker : playbackTarget, speakerVolume.load(), speakerBoost.load(),
      radioVolume.load(), radioBoost.load(), cuePlaying.load() ? 0 : playbackBytesPlayed.load(),
      cuePlaying.load() ? 0 : playbackBytesTotal.load(),
      playbackPaused.load() && state == PlaybackState::playing,
      state == PlaybackState::idle ? "" : cuePlaying.load() ? "Firmware cue" : playbackPath, playbackError};
}

void playbackStart(const String &path, PlaybackTarget target) {
  if (playbackGetState() != PlaybackState::idle || !playbackTask || !speakerReady) return;
  playbackTarget = target;
  path.toCharArray(playbackPath, sizeof(playbackPath));
  playbackError[0] = '\0';
  playbackBytesPlayed.store(0);
  playbackBytesTotal.store(0);
  playbackPaused.store(false);
  playbackSeekOffset.store(UINT32_MAX);
  playbackStopRequested.store(false);
  playbackState.store(PlaybackState::starting);
  xTaskNotifyGive(playbackTask);
}

bool playbackStartTone(const DacToneSettings &settings) {
  if (!speakerReady || !playbackTask || playbackGetState() != PlaybackState::idle ||
      (settings.sampleRate != 22050 && settings.sampleRate != 44100 && settings.sampleRate != 48000) ||
      (settings.bits != 16 && settings.bits != 32) || settings.frequency < 100 ||
      settings.frequency > 3000 || settings.level < 10 || settings.level > 90 ||
      (settings.durationSeconds != 0 && settings.durationSeconds != 10 &&
       settings.durationSeconds != 30 && settings.durationSeconds != 60)) return false;
  toneSettings = settings;
  playbackTarget = PlaybackTarget::radio;
  snprintf(playbackPath, sizeof(playbackPath), "DAC tone %u Hz / %lu Hz / %u bit / %s / %u%% / %s",
           settings.frequency, static_cast<unsigned long>(settings.sampleRate), settings.bits,
           settings.leftJustified ? "MSB" : "Philips", settings.level,
           settings.durationSeconds ? "timed" : "until stopped");
  playbackError[0] = '\0';
  playbackBytesPlayed.store(0);
  playbackBytesTotal.store(settings.sampleRate * settings.durationSeconds * 2 * (settings.bits / 8));
  playbackStopRequested.store(false);
  toneRequested.store(true);
  playbackState.store(PlaybackState::starting);
  xTaskNotifyGive(playbackTask);
  return true;
}

void playbackPlayCue(PlaybackCue cue) {
  if (!speakerReady || !playbackTask || playbackState.load() != PlaybackState::idle) return;
  bool expected = false;
  if (!cuePlaying.compare_exchange_strong(expected, true)) return;
  requestedCue.store(static_cast<uint8_t>(cue) + 1);
  xTaskNotifyGive(playbackTask);
}

bool playbackCueActive() { return cuePlaying.load(); }
bool playbackFileActive() { return playbackState.load() != PlaybackState::idle; }

uint8_t playbackCueSetting(bool automatic, bool microphone, bool started) {
  uint8_t mode = automatic ? (microphone ? 3 : 2) : (microphone ? 0 : 1);
  return (recordCueSettings.load() >> (mode * 4 + (started ? 0 : 2))) & 3;
}

bool playbackSaveCueSettings(uint16_t settings) {
  for (uint8_t shift = 0; shift < 16; shift += 2) if (((settings >> shift) & 3) > 2) return false;
  if (recordCueSettings.load() == settings) return true;
  Preferences preferences;
  if (!preferences.begin(kSpeakerSettingsNamespace, false)) return false;
  bool saved = preferences.putUShort(kRecordCueKey, settings) == sizeof(settings);
  preferences.end();
  if (saved) recordCueSettings.store(settings);
  return saved;
}

void playbackRecordCue(bool automatic, bool microphone, bool started) {
  uint8_t setting = playbackCueSetting(automatic, microphone, started);
  if (!setting) return;
  playbackPlayCue(setting == 1 ? started ? PlaybackCue::autoStarted : PlaybackCue::autoStopped :
    started ? PlaybackCue::recordingStarted : PlaybackCue::recordingStopped);
}

bool playbackStop() {
  if (playbackState.load() == PlaybackState::idle) return false;
  playbackStopRequested.store(true);
  return true;
}

bool playbackPause(bool paused) {
  if (playbackState.load() != PlaybackState::playing || cuePlaying.load() || toneRequested.load()) return false;
  playbackPaused.store(paused);
  return true;
}

bool playbackSeek(uint32_t offset) {
  uint32_t total = playbackBytesTotal.load();
  if (playbackState.load() != PlaybackState::playing || !total || offset >= total || cuePlaying.load()) return false;
  playbackSeekOffset.store(offset & ~1U);
  return true;
}

void playbackSetVolume(uint8_t volume) {
  if (speakerVolume.load() == volume) return;
  speakerVolume.store(volume);
  speakerSettingsChangedAt = millis();
  speakerSettingsDirty = true;
}

void playbackSetRadioVolume(uint8_t volume) {
  if (volume > 100 || radioVolume.load() == volume) return;
  radioVolume.store(volume);
  speakerSettingsChangedAt = millis();
  speakerSettingsDirty = true;
}

void playbackSetRadioBoost(uint8_t boost) {
  if ((boost != 1 && boost != 2 && boost != 4 && boost != 8 && boost != 16) || radioBoost.load() == boost) return;
  radioBoost.store(boost);
  speakerSettingsChangedAt = millis();
  speakerSettingsDirty = true;
}

void playbackSetBoost(uint8_t boost) {
  if (speakerBoost.load() == boost) return;
  speakerBoost.store(boost);
  speakerSettingsChangedAt = millis();
  speakerSettingsDirty = true;
}

void playbackPollSettings() {
  if (!speakerSettingsDirty || static_cast<uint32_t>(millis() - speakerSettingsChangedAt) < kSpeakerSaveDelayMs) return;
  uint8_t volume = speakerVolume.load();
  uint8_t boost = speakerBoost.load();
  uint8_t radio = radioVolume.load();
  uint8_t radioGain = radioBoost.load();
  if (volume == savedSpeakerVolume && boost == savedSpeakerBoost && radio == savedRadioVolume &&
      radioGain == savedRadioBoost) {
    speakerSettingsDirty = false;
    return;
  }
  Preferences preferences;
  if (!preferences.begin(kSpeakerSettingsNamespace, false)) {
    speakerSettingsChangedAt = millis();
    return;
  }
  if (volume == savedSpeakerVolume || preferences.putUChar(kSpeakerVolumeKey, volume) == sizeof(volume)) {
    savedSpeakerVolume = volume;
  }
  if (boost == savedSpeakerBoost || preferences.putUChar(kSpeakerBoostKey, boost) == sizeof(boost)) {
    savedSpeakerBoost = boost;
  }
  if (radio == savedRadioVolume || preferences.putUChar(kRadioVolumeKey, radio) == sizeof(radio)) {
    savedRadioVolume = radio;
  }
  if (radioGain == savedRadioBoost || preferences.putUChar(kRadioBoostKey, radioGain) == sizeof(radioGain)) {
    savedRadioBoost = radioGain;
  }
  preferences.end();
  speakerSettingsDirty = volume != savedSpeakerVolume || boost != savedSpeakerBoost ||
                         radio != savedRadioVolume || radioGain != savedRadioBoost;
  if (speakerSettingsDirty) speakerSettingsChangedAt = millis();
}

static bool initSpeaker();

static void configureDacTone(i2s_std_config_t &config, const DacToneSettings &settings) {
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(settings.sampleRate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (settings.bits == 32) config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  if (settings.leftJustified) {
    if (settings.bits == 16) config.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    else config.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  }
}

static esp_err_t startDacOutput(i2s_chan_handle_t &outputHandle, i2s_std_config_t &config) {
  i2s_del_channel(speakerHandle);
  speakerHandle = nullptr;
  speakerReady = false;
  esp_err_t result = gpio_reset_pin(kSpeakerBclkGpio);
  if (result == ESP_OK) result = gpio_reset_pin(kSpeakerLrcGpio);
  if (result == ESP_OK) result = gpio_reset_pin(kSpeakerDataGpio);
  if (result == ESP_OK) result = gpio_set_direction(kSpeakerBclkGpio, GPIO_MODE_INPUT);
  if (result == ESP_OK) result = gpio_set_direction(kSpeakerLrcGpio, GPIO_MODE_INPUT);
  if (result == ESP_OK) result = gpio_set_direction(kSpeakerDataGpio, GPIO_MODE_INPUT);
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (result == ESP_OK) result = i2s_new_channel(&channel, &outputHandle, nullptr);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = kDacBclkGpio;
  config.gpio_cfg.ws = kDacLrcGpio;
  config.gpio_cfg.dout = kDacDataGpio;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (result == ESP_OK) result = i2s_channel_init_std_mode(outputHandle, &config);
  if (result == ESP_OK) result = i2s_channel_enable(outputHandle);
  return result;
}

static void stopDacOutput(i2s_chan_handle_t outputHandle, bool enabled) {
  if (enabled) i2s_channel_disable(outputHandle);
  if (outputHandle) i2s_del_channel(outputHandle);
  speakerReady = initSpeaker();
  if (!speakerReady && !playbackError[0])
    snprintf(playbackError, sizeof(playbackError), "Speaker restore failed");
}

static void playDacTone() {
  DacToneSettings settings = toneSettings;
  i2s_chan_handle_t outputHandle = nullptr;
  i2s_std_config_t config = {};
  configureDacTone(config, settings);
  esp_err_t result = startDacOutput(outputHandle, config);
  bool enabled = result == ESP_OK;
  if (enabled) {
    playbackState.store(PlaybackState::playing);
    constexpr size_t kFrames = 128;
    int16_t output16[kFrames * 2];
    int32_t output32[kFrames * 2];
    uint32_t framesLeft = settings.sampleRate * settings.durationSeconds;
    float phase = 0;
    float phaseStep = 2.0f * PI * settings.frequency / settings.sampleRate;
    while ((settings.durationSeconds == 0 || framesLeft) && !playbackStopRequested.load()) {
      watchdogResetCurrentTask();
      size_t frames = settings.durationSeconds ? min(static_cast<size_t>(framesLeft), kFrames) : kFrames;
      for (size_t index = 0; index < frames; ++index) {
        int16_t sample = static_cast<int16_t>(sinf(phase) * 32767.0f * settings.level / 100.0f);
        phase += phaseStep;
        if (phase >= 2.0f * PI) phase -= 2.0f * PI;
        output16[index * 2] = output16[index * 2 + 1] = sample;
        output32[index * 2] = output32[index * 2 + 1] = static_cast<int32_t>(sample) * 65536;
      }
      size_t bytes = frames * 2 * (settings.bits / 8);
      size_t written = 0;
      result = i2s_channel_write(outputHandle, settings.bits == 16 ? static_cast<void *>(output16) :
                                static_cast<void *>(output32), bytes, &written, 1000);
      if (result != ESP_OK || written != bytes) {
        snprintf(playbackError, sizeof(playbackError), "DAC tone write: %s (%zu/%zu)",
                 esp_err_to_name(result), written, bytes);
        break;
      }
      uint32_t played = playbackBytesPlayed.load();
      playbackBytesPlayed.store(played > UINT32_MAX - bytes ? UINT32_MAX : played + bytes);
      if (settings.durationSeconds) framesLeft -= frames;
    }
  } else {
    snprintf(playbackError, sizeof(playbackError), "DAC tone start: %s", esp_err_to_name(result));
  }
  stopDacOutput(outputHandle, enabled);
}

static void speakerLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t cue = requestedCue.exchange(0);
    if (cue) {
      bool cueWatchdog = watchdogSubscribeCurrentTask();
      playCue(static_cast<PlaybackCue>(cue - 1));
      watchdogUnsubscribeCurrentTask(cueWatchdog);
      cuePlaying.store(false);
      continue;
    }
    if (playbackState.load() != PlaybackState::starting) continue;
    bool playbackWatchdog = watchdogSubscribeCurrentTask();
    if (toneRequested.exchange(false)) {
      playDacTone();
      playbackState.store(PlaybackState::idle);
      watchdogUnsubscribeCurrentTask(playbackWatchdog);
      continue;
    }
    bool radio = playbackTarget == PlaybackTarget::radio;
    i2s_chan_handle_t outputHandle = radio ? nullptr : speakerHandle;
    i2s_std_config_t dacConfig = {};
    File file = SD_MMC.open(playbackPath, FILE_READ);
    uint32_t remaining = 0;
    bool enabled = false;
    if (!file || !findPcmData(file, remaining)) {
      snprintf(playbackError, sizeof(playbackError), "Cannot read PCM WAV");
    } else if (!playbackStopRequested.load()) {
      playbackBytesTotal.store(remaining);
      esp_err_t enableResult;
      if (radio) {
        dacConfig.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
        dacConfig.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        enableResult = startDacOutput(outputHandle, dacConfig);
      } else {
        enableResult = i2s_channel_enable(outputHandle);
      }
      enabled = enableResult == ESP_OK;
      if (!enabled) {
        snprintf(playbackError, sizeof(playbackError), radio ? "Cannot start radio DAC: %s" : "Cannot start speaker: %s",
                 esp_err_to_name(enableResult));
      } else {
        playbackState.store(PlaybackState::playing);
        uint32_t dataOffset = file.position();
        uint8_t input[512];
        int16_t output[512];
        int16_t radioOutput[1024];
        float appliedGain = 0;
        float appliedRadioGain = 0;
        bool radioFirstBlockReported = false;
        while (remaining && !playbackStopRequested.load()) {
          if (playbackWatchdog) watchdogResetCurrentTask();
          uint32_t seek = playbackSeekOffset.exchange(UINT32_MAX);
          if (seek != UINT32_MAX) {
            if (!file.seek(dataOffset + seek)) { snprintf(playbackError, sizeof(playbackError), "WAV seek failed"); break; }
            remaining = playbackBytesTotal.load() - seek;
            playbackBytesPlayed.store(seek);
          }
          if (playbackPaused.load()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
          size_t count = file.read(input, min(static_cast<uint32_t>(sizeof(input)), remaining));
          if (!count || (count & 1)) {
            snprintf(playbackError, sizeof(playbackError), "WAV read failed");
            break;
          }
          remaining -= count;
          int32_t peak = 0;
          for (size_t index = 0; index < count / 2; ++index) {
            int32_t sample = static_cast<int16_t>(wav16(input + index * 2));
            peak = max(peak, abs(sample));
          }
          float requestedGain = speakerVolume.load() * speakerBoost.load() / 100.0f;
          float limitedGain = peak ? min(requestedGain, 32767.0f / peak) : requestedGain;
          appliedGain = min(limitedGain, appliedGain + 0.1f);
          float requestedRadioGain = radioVolume.load() * radioBoost.load() / 100.0f;
          float maxOutput = 32767.0f * radioVolume.load() / 100.0f;
          float limitedRadioGain = peak ? fminf(requestedRadioGain, maxOutput / peak) : requestedRadioGain;
          appliedRadioGain = fminf(limitedRadioGain, appliedRadioGain + 0.25f);
          for (size_t index = 0; index < count / 2; ++index) {
            int16_t source = static_cast<int16_t>(wav16(input + index * 2));
            if (radio) {
              int16_t sample = static_cast<int16_t>(source * appliedRadioGain);
              radioOutput[index * 4] = sample;
              radioOutput[index * 4 + 1] = sample;
              radioOutput[index * 4 + 2] = sample;
              radioOutput[index * 4 + 3] = sample;
            } else {
              int16_t sample = constrain(static_cast<int32_t>(source * appliedGain), -32767, 32767);
              output[index * 2] = sample;
              output[index * 2 + 1] = sample;
            }
          }
          size_t written = 0;
          size_t bytes = count * (radio ? 4 : 2);
          esp_err_t result = i2s_channel_write(outputHandle,
                                               radio ? static_cast<void *>(radioOutput) : static_cast<void *>(output),
                                               bytes, &written, 1000);
          if (result != ESP_OK || written != bytes) {
            snprintf(playbackError, sizeof(playbackError), radio ? "Radio DAC output failed: %s (%zu/%zu)" :
                     "Speaker output failed: %s (%zu/%zu)", esp_err_to_name(result), written, bytes);
            Serial.printf("I2S %s write: %s (0x%x), %zu/%zu bytes\n", radio ? "radio" : "speaker",
                          esp_err_to_name(result), static_cast<unsigned>(result), written, bytes);
            break;
          }
          playbackBytesPlayed.fetch_add(count);
          if (radio && !radioFirstBlockReported) {
            Serial.printf("Radio DAC first block: %zu I2S bytes\n", written);
            radioFirstBlockReported = true;
          }
        }
        if (!remaining && !playbackStopRequested.load() && !playbackError[0]) {
          int32_t silence[512] = {};
          for (int index = 0; index < 4; ++index) {
            size_t written = 0;
            size_t bytes = radio ? sizeof(silence) : sizeof(silence) / 2;
            if (i2s_channel_write(outputHandle, silence, bytes, &written, 1000) != ESP_OK) break;
          }
          vTaskDelay(pdMS_TO_TICKS(30));
        }
      }
    }
    if (enabled) {
      if (!radio) i2s_channel_disable(outputHandle);
    }
    if (radio && !speakerReady) stopDacOutput(outputHandle, enabled);
    if (file) file.close();
    playbackPaused.store(false);
    playbackSeekOffset.store(UINT32_MAX);
    playbackState.store(PlaybackState::idle);
    watchdogUnsubscribeCurrentTask(playbackWatchdog);
  }
}

static bool initSpeaker() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel, &speakerHandle, nullptr) != ESP_OK) return false;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = kSpeakerBclkGpio;
  config.gpio_cfg.ws = kSpeakerLrcGpio;
  config.gpio_cfg.dout = kSpeakerDataGpio;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(speakerHandle, &config) == ESP_OK) return true;
  i2s_del_channel(speakerHandle);
  speakerHandle = nullptr;
  return false;
}

void initPlayback(bool sdReady) {
  Preferences preferences;
  if (preferences.begin(kSpeakerSettingsNamespace, true)) {
    uint8_t volume = preferences.getUChar(kSpeakerVolumeKey, savedSpeakerVolume);
    uint8_t boost = preferences.getUChar(kSpeakerBoostKey, savedSpeakerBoost);
    uint8_t radio = preferences.getUChar(kRadioVolumeKey, savedRadioVolume);
    uint8_t radioGain = preferences.getUChar(kRadioBoostKey, savedRadioBoost);
    uint16_t cues = preferences.getUShort(kRecordCueKey, recordCueSettings.load());
    bool validCues = true;
    for (uint8_t shift = 0; shift < 16; shift += 2) if (((cues >> shift) & 3) > 2) validCues = false;
    if (validCues) recordCueSettings.store(cues);
    if (volume <= 100) savedSpeakerVolume = volume;
    if (boost == 1 || boost == 2 || boost == 4) savedSpeakerBoost = boost;
    if (radio <= 100) savedRadioVolume = radio;
    if (radioGain == 1 || radioGain == 2 || radioGain == 4 || radioGain == 8 || radioGain == 16)
      savedRadioBoost = radioGain;
    preferences.end();
  }
  speakerVolume.store(savedSpeakerVolume);
  speakerBoost.store(savedSpeakerBoost);
  radioVolume.store(savedRadioVolume);
  radioBoost.store(savedRadioBoost);
  speakerReady = initSpeaker();
  if (speakerReady &&
      xTaskCreatePinnedToCore(speakerLoop, "speaker", 12288, nullptr, 2, &playbackTask, 0) != pdPASS) {
    Serial.println("Cannot create playback task");
  }
  if (!sdReady) Serial.println("Speaker cues available without SD; file playback disabled");
}