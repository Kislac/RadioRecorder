#include "watchdog.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

namespace {
constexpr uint32_t kWatchdogTimeoutMs = 10000;
constexpr uint32_t kWatchdogIdleCoreMask = (1U << portNUM_PROCESSORS) - 1;

esp_task_wdt_config_t watchdogConfig() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = kWatchdogTimeoutMs;
  config.idle_core_mask = kWatchdogIdleCoreMask;
  config.trigger_panic = true;
  return config;
}
}

bool initSystemWatchdog() {
  esp_task_wdt_config_t config = watchdogConfig();
  esp_err_t result = esp_task_wdt_init(&config);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_reconfigure(&config);
  if (result != ESP_OK) return false;

  result = esp_task_wdt_status(nullptr);
  if (result == ESP_ERR_NOT_FOUND) result = esp_task_wdt_add(nullptr);
  return result == ESP_OK;
}

void feedSystemWatchdog() { esp_task_wdt_reset(); }

bool watchdogSubscribeCurrentTask() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) return false;
  return esp_task_wdt_add(nullptr) == ESP_OK;
}

void watchdogUnsubscribeCurrentTask(bool subscribedHere) {
  if (subscribedHere) esp_task_wdt_delete(nullptr);
}

void watchdogResetCurrentTask() { esp_task_wdt_reset(); }