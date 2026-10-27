#include <Arduino.h>
#include "controls.h"
#include "app_control.h"
#include "recorder.h"
#include "web_server.h"

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
    encoderSteps = encoderSteps + 1;
    encoderQuarterSteps = 0;
  } else if (encoderQuarterSteps <= -4) {
    encoderSteps = encoderSteps - 1;
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

void pollEncoderRotation() {
  static uint32_t nextActionAt = 0;
  portENTER_CRITICAL(&encoderMux);
  int32_t steps = encoderSteps;
  encoderSteps = 0;
  portEXIT_CRITICAL(&encoderMux);
  if (!steps || static_cast<int32_t>(millis() - nextActionAt) < 0) return;
  nextActionAt = millis() + 300;
  if (steps > 0) {
    if (appWifiTestActive()) {
      Serial.println("Zajteszt alatt a felvetel nem kapcsolhato");
    } else if (recorderState() == CaptureState::idle) {
      Serial.println(appStartRecording() ? String("Felvetel indul: ") + recorderName() : String("Felvetel nem indithato: ") + recorderError());
    } else if (recorderStopping()) {
      Serial.println("Felvetel leallitasa folyamatban");
    } else {
      Serial.println(appStopRecording() ? "Felvetel leallitasa kerese" : "Felvetel nem allithato le");
    }
  } else {
    recorderToggleSerialMeter();
  }
}

void pollEncoderButton() {
  static bool lastReading = HIGH;
  static bool stable = HIGH;
  static uint32_t changedAt = 0;
  bool reading = digitalRead(kEncoderSwGpio);
  if (reading != lastReading) changedAt = millis();
  lastReading = reading;
  if (reading != stable && millis() - changedAt >= 30) {
    stable = reading;
    if (stable == LOW) toggleWifi();
  }
}