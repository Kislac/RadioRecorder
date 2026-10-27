#include "playback.h"

#include <SD_MMC.h>
#include <atomic>
#include "driver/i2s_std.h"
#include "wav.h"

static constexpr gpio_num_t kSpeakerBclkGpio = GPIO_NUM_42;
static constexpr gpio_num_t kSpeakerLrcGpio = GPIO_NUM_41;
static constexpr gpio_num_t kSpeakerDataGpio = GPIO_NUM_11;
static i2s_chan_handle_t speakerHandle = nullptr;
static std::atomic<PlaybackState> playbackState{PlaybackState::idle};
static std::atomic<bool> playbackStopRequested{false};
static std::atomic<uint8_t> speakerVolume{25};
static std::atomic<uint8_t> speakerBoost{1};
static TaskHandle_t playbackTask = nullptr;
static char playbackPath[221] = {};
static char playbackError[80] = {};
static bool speakerReady = false;

PlaybackState playbackGetState() {
  return playbackState.load();
}

PlaybackStatus playbackStatus() {
  PlaybackState state = playbackState.load();
  return {state, speakerReady && playbackTask != nullptr, speakerVolume.load(), speakerBoost.load(),
          state == PlaybackState::idle ? "" : playbackPath, playbackError};
}

void playbackStart(const String &path) {
  path.toCharArray(playbackPath, sizeof(playbackPath));
  playbackError[0] = '\0';
  playbackStopRequested.store(false);
  playbackState.store(PlaybackState::starting);
  xTaskNotifyGive(playbackTask);
}

bool playbackStop() {
  if (playbackState.load() == PlaybackState::idle) return false;
  playbackStopRequested.store(true);
  return true;
}

void playbackSetVolume(uint8_t volume) {
  speakerVolume.store(volume);
}

void playbackSetBoost(uint8_t boost) {
  speakerBoost.store(boost);
}

static void speakerLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (playbackState.load() != PlaybackState::starting) continue;
    File file = SD_MMC.open(playbackPath, FILE_READ);
    uint32_t remaining = 0;
    bool enabled = false;
    if (!file || !findPcmData(file, remaining)) {
      snprintf(playbackError, sizeof(playbackError), "Cannot read PCM WAV");
    } else if (!playbackStopRequested.load()) {
      enabled = i2s_channel_enable(speakerHandle) == ESP_OK;
      if (!enabled) {
        snprintf(playbackError, sizeof(playbackError), "Cannot start speaker");
      } else {
        playbackState.store(PlaybackState::playing);
        uint8_t input[512];
        int16_t output[512];
        float appliedGain = 0;
        while (remaining && !playbackStopRequested.load()) {
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
            int16_t sample = constrain(static_cast<int32_t>(static_cast<int16_t>(wav16(input + index * 2)) * appliedGain),
                                       -32767, 32767);
            output[index * 2] = sample;
            output[index * 2 + 1] = sample;
          }
          size_t written = 0;
          if (i2s_channel_write(speakerHandle, output, count * 2, &written, 1000) != ESP_OK || written != count * 2) {
            snprintf(playbackError, sizeof(playbackError), "Speaker output failed");
            break;
          }
        }
        if (!remaining && !playbackStopRequested.load() && !playbackError[0]) {
          int16_t silence[512] = {};
          for (int index = 0; index < 4; ++index) {
            size_t written = 0;
            if (i2s_channel_write(speakerHandle, silence, sizeof(silence), &written, 1000) != ESP_OK) break;
          }
          vTaskDelay(pdMS_TO_TICKS(30));
        }
      }
    }
    if (enabled) i2s_channel_disable(speakerHandle);
    if (file) file.close();
    playbackState.store(PlaybackState::idle);
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
  speakerReady = initSpeaker();
  if (sdReady && speakerReady && xTaskCreatePinnedToCore(speakerLoop, "speaker", 12288, nullptr, 2, &playbackTask, 0) != pdPASS) {
    Serial.println("Cannot create speaker task");
  }
}