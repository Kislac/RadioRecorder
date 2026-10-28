#include "ptt_test.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>

namespace {
constexpr uint8_t kRelayGpio = 10;
constexpr uint8_t kRelayActiveLevel = LOW;
constexpr uint8_t kRelayIdleLevel = HIGH;
bool initialized = false;
std::atomic<bool> active{false};
std::atomic<bool> latched{false};
uint32_t startedAt = 0;
esp_timer_handle_t pulseTimer = nullptr;
}

void initPttTest() {
  digitalWrite(kRelayGpio, kRelayIdleLevel);
  pinMode(kRelayGpio, OUTPUT);
  active = false;
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = [](void *) {
    digitalWrite(kRelayGpio, kRelayIdleLevel);
    active = false;
  };
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "ptt-test";
  initialized = esp_timer_create(&timerArgs, &pulseTimer) == ESP_OK;
  if (!initialized) Serial.println("PTT test: timer unavailable; relay disabled");
}

void stopPttTest() {
  active = false;
  latched = false;
  if (initialized) digitalWrite(kRelayGpio, kRelayIdleLevel);
  if (pulseTimer) esp_timer_stop(pulseTimer);
}

void pollPttTest() {
  if (active && !latched && static_cast<uint32_t>(millis() - startedAt) >= kPttTestPulseMs) stopPttTest();
}

bool pttTestActive() {
  pollPttTest();
  return active;
}

bool startPttTest() {
  pollPttTest();
  if (!initialized || active) return false;
  startedAt = millis();
  active = true;
  digitalWrite(kRelayGpio, kRelayActiveLevel);
  if (esp_timer_start_once(pulseTimer, static_cast<uint64_t>(kPttTestPulseMs) * 1000) != ESP_OK) {
    stopPttTest();
    return false;
  }
  return true;
}

bool latchPttTest() {
  pollPttTest();
  if (!initialized || active) return false;
  latched = true;
  active = true;
  digitalWrite(kRelayGpio, kRelayActiveLevel);
  return true;
}