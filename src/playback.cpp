#include "playback.h"

#include <SD_MMC.h>
#include <Preferences.h>
#include <atomic>
#include <cmath>
#include "driver/i2s_std.h"
#include "recorder.h"
#include "wav.h"
#include "watchdog.h"

static constexpr gpio_num_t kSpeakerBclkGpio = GPIO_NUM_42;
static constexpr gpio_num_t kSpeakerLrcGpio = GPIO_NUM_41;
static constexpr gpio_num_t kSpeakerDataGpio = GPIO_NUM_11;
static i2s_chan_handle_t speakerHandle = nullptr;
static std::atomic<PlaybackState> playbackState{PlaybackState::idle};
static std::atomic<bool> playbackStopRequested{false};
static std::atomic<bool> cuePlaying{false};
static std::atomic<uint8_t> requestedCue{0};
static std::atomic<uint8_t> speakerVolume{25};
static std::atomic<uint8_t> speakerBoost{1};
static constexpr char kSpeakerSettingsNamespace[] = "radio-recorder";
static constexpr char kSpeakerVolumeKey[] = "spkVol";
static constexpr char kSpeakerBoostKey[] = "spkBoost";
static constexpr uint32_t kSpeakerSaveDelayMs = 1000;
static uint8_t savedSpeakerVolume = 25;
static uint8_t savedSpeakerBoost = 1;
static uint32_t speakerSettingsChangedAt = 0;
static bool speakerSettingsDirty = false;
static TaskHandle_t playbackTask = nullptr;
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
  }
}

PlaybackState playbackGetState() {
  return cuePlaying.load() ? PlaybackState::playing : playbackState.load();
}

PlaybackStatus playbackStatus() {
  PlaybackState state = playbackGetState();
  return {state, speakerReady && playbackTask != nullptr, recorderRadioHandle() && playbackTask != nullptr,
      cuePlaying.load() ? PlaybackTarget::speaker : playbackTarget, speakerVolume.load(), speakerBoost.load(),
          state == PlaybackState::idle ? "" : cuePlaying.load() ? "Firmware cue" : playbackPath, playbackError};
}

void playbackStart(const String &path, PlaybackTarget target) {
  if (playbackGetState() != PlaybackState::idle || !playbackTask ||
      (target == PlaybackTarget::radio ? !recorderRadioHandle() : !speakerReady)) return;
  playbackTarget = target;
  path.toCharArray(playbackPath, sizeof(playbackPath));
  playbackError[0] = '\0';
  playbackStopRequested.store(false);
  playbackState.store(PlaybackState::starting);
  xTaskNotifyGive(playbackTask);
}

void playbackPlayCue(PlaybackCue cue) {
  if (!speakerReady || !playbackTask || playbackState.load() != PlaybackState::idle) return;
  bool expected = false;
  if (!cuePlaying.compare_exchange_strong(expected, true)) return;
  requestedCue.store(static_cast<uint8_t>(cue) + 1);
  xTaskNotifyGive(playbackTask);
}

bool playbackStop() {
  if (playbackState.load() == PlaybackState::idle) return false;
  playbackStopRequested.store(true);
  return true;
}

void playbackSetVolume(uint8_t volume) {
  if (speakerVolume.load() == volume) return;
  speakerVolume.store(volume);
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
  if (volume == savedSpeakerVolume && boost == savedSpeakerBoost) { speakerSettingsDirty = false; return; }
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
  preferences.end();
  speakerSettingsDirty = volume != savedSpeakerVolume || boost != savedSpeakerBoost;
  if (speakerSettingsDirty) speakerSettingsChangedAt = millis();
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
    bool radio = playbackTarget == PlaybackTarget::radio;
    i2s_chan_handle_t outputHandle = radio ? recorderRadioHandle() : speakerHandle;
    File file = SD_MMC.open(playbackPath, FILE_READ);
    uint32_t remaining = 0;
    bool enabled = false;
    if (!file || !findPcmData(file, remaining)) {
      snprintf(playbackError, sizeof(playbackError), "Cannot read PCM WAV");
    } else if (!playbackStopRequested.load()) {
      enabled = i2s_channel_enable(outputHandle) == ESP_OK;
      if (!enabled) {
        snprintf(playbackError, sizeof(playbackError), radio ? "Cannot start radio DAC" : "Cannot start speaker");
      } else {
        playbackState.store(PlaybackState::playing);
        uint8_t input[512];
        int16_t output[512];
        int32_t radioOutput[512];
        float appliedGain = 0;
        while (remaining && !playbackStopRequested.load()) {
          if (playbackWatchdog) watchdogResetCurrentTask();
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
          for (size_t index = 0; index < count / 2; ++index) {
            int16_t source = static_cast<int16_t>(wav16(input + index * 2));
            if (radio) {
              int32_t sample = static_cast<int32_t>(source * 0.05f * 65536.0f);
              radioOutput[index * 2] = sample;
              radioOutput[index * 2 + 1] = sample;
            } else {
              int16_t sample = constrain(static_cast<int32_t>(source * appliedGain), -32767, 32767);
              output[index * 2] = sample;
              output[index * 2 + 1] = sample;
            }
          }
          size_t written = 0;
          size_t bytes = count * (radio ? 4 : 2);
          if (i2s_channel_write(outputHandle, radio ? static_cast<void *>(radioOutput) : static_cast<void *>(output),
                                bytes, &written, 1000) != ESP_OK || written != bytes) {
            snprintf(playbackError, sizeof(playbackError), radio ? "Radio DAC output failed" : "Speaker output failed");
            break;
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
    if (enabled) i2s_channel_disable(outputHandle);
    if (file) file.close();
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
    if (volume <= 100) savedSpeakerVolume = volume;
    if (boost == 1 || boost == 2 || boost == 4) savedSpeakerBoost = boost;
    preferences.end();
  }
  speakerVolume.store(savedSpeakerVolume);
  speakerBoost.store(savedSpeakerBoost);
  speakerReady = initSpeaker();
  if ((speakerReady || recorderRadioHandle()) &&
      xTaskCreatePinnedToCore(speakerLoop, "speaker", 12288, nullptr, 2, &playbackTask, 0) != pdPASS) {
    Serial.println("Cannot create playback task");
  }
  if (!sdReady) Serial.println("Speaker cues available without SD; file playback disabled");
}