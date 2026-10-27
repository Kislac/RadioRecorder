#include <Arduino.h>
#include "controls.h"

static constexpr int kEncoderClkGpio = 21;
static constexpr int kEncoderDtGpio = 47;
static constexpr int kEncoderSwGpio = 14;
static portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t encoderState = 0;
static volatile int8_t encoderQuarterSteps = 0;
static volatile int32_t encoderSteps = 0;

static void IRAM_ATTR onEncoderEdge() {
  static const DRAM_ATTR int8_t transitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t state = (digitalRead(kEncoderClkGpio) << 1) | digitalRead(kEncoderDtGpio);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderQuarterSteps += transitions[(encoderState << 2) | state];
  encoderState = state;
  if (encoderQuarterSteps >= 4) {
    encoderSteps = encoderSteps - 1;
    encoderQuarterSteps = 0;
  } else if (encoderQuarterSteps <= -4) {
    encoderSteps = encoderSteps + 1;
    encoderQuarterSteps = 0;
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void initControls() {
  pinMode(kEncoderClkGpio, INPUT_PULLUP);
  pinMode(kEncoderDtGpio, INPUT_PULLUP);
  pinMode(kEncoderSwGpio, INPUT_PULLUP);
  encoderState = (digitalRead(kEncoderClkGpio) << 1) | digitalRead(kEncoderDtGpio);
  attachInterrupt(digitalPinToInterrupt(kEncoderClkGpio), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncoderDtGpio), onEncoderEdge, CHANGE);
}

int32_t takeEncoderSteps() {
  portENTER_CRITICAL(&encoderMux);
  int32_t steps = encoderSteps;
  encoderSteps = 0;
  portEXIT_CRITICAL(&encoderMux);
  return steps;
}

ButtonEvent pollEncoderButton() {
  static bool lastReading = HIGH;
  static bool stable = HIGH;
  static uint32_t changedAt = 0;
  static uint32_t pressedAt = 0;
  static bool longHandled = false;
  bool reading = digitalRead(kEncoderSwGpio);
  if (reading != lastReading) changedAt = millis();
  lastReading = reading;
  if (reading != stable && millis() - changedAt >= 30) {
    stable = reading;
    if (stable == LOW) {
      pressedAt = millis();
      longHandled = false;
    } else if (!longHandled) {
      return ButtonEvent::shortPress;
    }
  }
  if (stable == LOW && !longHandled && millis() - pressedAt >= 700) {
    longHandled = true;
    return ButtonEvent::longPress;
  }
  return ButtonEvent::none;
}