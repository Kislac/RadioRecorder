#include "ptt_test.h"
#include "radio_programming.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>

namespace {
constexpr uint8_t kRelayGpio = 10;
constexpr uint8_t kRelayActiveLevel = LOW;
constexpr uint8_t kRelayIdleLevel = HIGH;
enum class PttOwner { none, test, playback };
portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;
PttOwner owner = PttOwner::none;
bool initialized = false;
bool latched = false;
uint32_t startedAt = 0;
esp_timer_handle_t pulseTimer = nullptr;
esp_timer_handle_t playbackTimer = nullptr;
}

void initPttTest() {
  gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);
  pinMode(kRelayGpio, OUTPUT);
  digitalWrite(kRelayGpio, kRelayIdleLevel);
  owner = PttOwner::none;
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = [](void *) {
    portENTER_CRITICAL(&relayMux);
    if (owner == PttOwner::test && !latched) {
      gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);
      owner = PttOwner::none;
    }
    portEXIT_CRITICAL(&relayMux);
  };
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "ptt-test";
  initialized = esp_timer_create(&timerArgs, &pulseTimer) == ESP_OK;
  timerArgs.callback = [](void *) {
    portENTER_CRITICAL(&relayMux);
    if (owner == PttOwner::playback) {
      gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);
      owner = PttOwner::none;
    }
    portEXIT_CRITICAL(&relayMux);
  };
  timerArgs.name = "ptt-playback";
  initialized = initialized && esp_timer_create(&timerArgs, &playbackTimer) == ESP_OK;
  if (!initialized) Serial.println("PTT test: timer unavailable; relay disabled");
}

void stopPttTest() {
  portENTER_CRITICAL(&relayMux);
  if (owner == PttOwner::test) {
    gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);
    owner = PttOwner::none;
    latched = false;
  }
  portEXIT_CRITICAL(&relayMux);
  if (pulseTimer) esp_timer_stop(pulseTimer);
}

void pollPttTest() {
  portENTER_CRITICAL(&relayMux);
  bool expired = owner == PttOwner::test && !latched && static_cast<uint32_t>(millis() - startedAt) >= kPttTestPulseMs;
  portEXIT_CRITICAL(&relayMux);
  if (expired) stopPttTest();
}

bool pttTestActive() {
  pollPttTest();
  portENTER_CRITICAL(&relayMux);
  bool active = owner != PttOwner::none;
  portEXIT_CRITICAL(&relayMux);
  return active;
}

bool startPttTest() {
  pollPttTest();
  if (radioProgrammingBusy()) return false;
  portENTER_CRITICAL(&relayMux);
  if (!initialized || owner != PttOwner::none) { portEXIT_CRITICAL(&relayMux); return false; }
  startedAt = millis();
  owner = PttOwner::test;
  latched = false;
  gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayActiveLevel);
  portEXIT_CRITICAL(&relayMux);
  if (esp_timer_start_once(pulseTimer, static_cast<uint64_t>(kPttTestPulseMs) * 1000) != ESP_OK) {
    stopPttTest();
    return false;
  }
  return true;
}

bool latchPttTest() {
  pollPttTest();
  if (radioProgrammingBusy()) return false;
  portENTER_CRITICAL(&relayMux);
  if (!initialized || owner != PttOwner::none) { portEXIT_CRITICAL(&relayMux); return false; }
  latched = true;
  owner = PttOwner::test;
  gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayActiveLevel);
  portEXIT_CRITICAL(&relayMux);
  return true;
}

bool acquirePlaybackPtt(uint32_t maxKeyMs) {
  if (radioProgrammingBusy()) return false;
  portENTER_CRITICAL(&relayMux);
  if (!initialized || owner != PttOwner::none) { portEXIT_CRITICAL(&relayMux); return false; }
  owner = PttOwner::playback;
  portEXIT_CRITICAL(&relayMux);
  if (!maxKeyMs || esp_timer_start_once(playbackTimer, static_cast<uint64_t>(maxKeyMs) * 1000) != ESP_OK) {
    releasePlaybackPtt();
    return false;
  }
  portENTER_CRITICAL(&relayMux);
  if (owner == PttOwner::playback) gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayActiveLevel);
  portEXIT_CRITICAL(&relayMux);
  return true;
}

bool playbackPttActive() {
  portENTER_CRITICAL(&relayMux);
  bool active = owner == PttOwner::playback;
  portEXIT_CRITICAL(&relayMux);
  return active;
}

void releasePlaybackPtt() {
  portENTER_CRITICAL(&relayMux);
  if (owner == PttOwner::playback) {
    gpio_set_level(static_cast<gpio_num_t>(kRelayGpio), kRelayIdleLevel);
    owner = PttOwner::none;
  }
  portEXIT_CRITICAL(&relayMux);
  if (playbackTimer) esp_timer_stop(playbackTimer);
}