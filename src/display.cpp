#include "display.h"

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "startup_image.h"
#include "app_control.h"
#include "playback.h"
#include "recorder.h"
#include "web_server.h"

#ifndef RADIORECORDER_OLED_ENABLED
#define RADIORECORDER_OLED_ENABLED 0
#endif

namespace {
constexpr int kSdaGpio = 4;
constexpr int kSclGpio = 5;
constexpr uint8_t kTileCount = 7;
constexpr uint8_t kSettingsCount = 7;
const char *const kTileNames[kTileCount] = {"Felvetel", "Fajlok", "Lejatszo", "Elo jel", "Diagnosztika", "Radio", "Beallitasok"};

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, kSclGpio, kSdaGpio);
bool ready = false;
uint8_t tile = 0;
uint8_t setting = 0;
uint8_t spinner = 0;
uint32_t lastBodyAt = 0;
uint32_t lastSpinnerAt = 0;
uint32_t lastActivityAt = 0;
uint32_t lastSecond = UINT32_MAX;
bool inSettings = false;
bool editing = false;
bool messagePending = false;
bool screenSleeping = false;
char message[26] = {};

void setMessage(const char *text) {
  snprintf(message, sizeof(message), "%s", text);
  messagePending = true;
}

void drawStatus(uint32_t seconds) {
  static const uint8_t arcMask[] = {0x2, 0x6, 0x4, 0xc, 0x8, 0x9, 0x1, 0x3};
  oled.setDrawColor(0);
  oled.drawBox(0, 0, 128, 10);
  oled.setDrawColor(1);
  uint8_t arcs = arcMask[spinner];
  if (arcs & 0x1) oled.drawCircle(4, 4, 3, U8G2_DRAW_UPPER_RIGHT);
  if (arcs & 0x2) oled.drawCircle(4, 4, 3, U8G2_DRAW_UPPER_LEFT);
  if (arcs & 0x4) oled.drawCircle(4, 4, 3, U8G2_DRAW_LOWER_LEFT);
  if (arcs & 0x8) oled.drawCircle(4, 4, 3, U8G2_DRAW_LOWER_RIGHT);
  oled.drawCircle(17, 8, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawCircle(17, 8, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  oled.drawDisc(17, 8, 1);
  if (!webReady()) oled.drawLine(12, 1, 22, 9);
  oled.setFont(u8g2_font_5x8_tf);
  if (recorderState() != CaptureState::idle) oled.drawStr(26, 8, "REC");
  uint32_t time = seconds;
  char clockText[24];
  snprintf(clockText, sizeof(clockText), "D %02lu:%02lu:%02lu", static_cast<unsigned long>(time / 3600),
           static_cast<unsigned long>((time / 60) % 60), static_cast<unsigned long>(time % 60));
  oled.drawStr(72, 8, clockText);
  oled.drawHLine(0, 10, 128);
}

void drawSettingValue(uint8_t y, const char *value) {
  oled.drawStr(127 - oled.getStrWidth(value), y, value);
}

void drawSettings() {
  oled.setFont(u8g2_font_5x8_tf);
  for (uint8_t index = 0; index < kSettingsCount; ++index) {
    uint8_t row = (setting / 4) * 4;
    if (index < row || index >= row + 4) continue;
    uint8_t y = 30 + (index - row) * 8;
    oled.drawStr(0, y, setting == index ? (editing ? "*" : ">") : " ");
    switch (index) {
      case 0: oled.drawStr(9, y, "Wi-Fi"); drawSettingValue(y, webReady() ? "BE" : "KI"); break;
      case 1: oled.drawStr(9, y, "Soros RMS"); drawSettingValue(y, recorderSerialMeterEnabled() ? "BE" : "KI"); break;
      case 2: oled.drawStr(9, y, "Hangero"); {
        char value[5];
        snprintf(value, sizeof(value), "%u%%", playbackStatus().volume);
        drawSettingValue(y, value);
        break;
      }
      case 3: oled.drawStr(9, y, "Boost"); {
        char value[5];
        snprintf(value, sizeof(value), "%ux", playbackStatus().boost);
        drawSettingValue(y, value);
        break;
      }
      case 4: oled.drawStr(9, y, "Low-pass"); drawSettingValue(y, recorderLowpass() ? "BE" : "KI"); break;
      case 5: oled.drawStr(9, y, "Kijelzo ido"); {
        char value[10];
        uint16_t minutes = appScreenOffMinutes();
        if (minutes == 0) snprintf(value, sizeof(value), "0 min");
        else snprintf(value, sizeof(value), "%u min", minutes);
        drawSettingValue(y, value);
        break;
      }
      default: oled.drawStr(9, y, "Vissza"); break;
    }
  }
}

void drawBody(bool fullUpdate = true) {
  oled.clearBuffer();
  drawStatus(appDemoTimeSeconds());
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 22, inSettings ? "Beallitasok" : kTileNames[tile]);
  oled.setFont(u8g2_font_5x8_tf);
  if (inSettings) {
    drawSettings();
  } else if (tile == 0) {
    CaptureState state = recorderState();
    oled.drawStr(0, 34, state == CaptureState::idle ? "All: nyomas = indit" :
                 recorderStopping() ? "Lezaras..." : state == CaptureState::starting ? "Indulas..." : "REC: nyomas = stop");
    if (state != CaptureState::idle) oled.drawStr(0, 44, recorderName());
  } else if (tile == 2) {
    PlaybackStatus playback = playbackStatus();
    oled.drawStr(0, 35, playback.state == PlaybackState::idle ? "Nincs lejatszas" : "Lejatszas folyamatban");
    oled.drawStr(0, 48, playback.state == PlaybackState::idle ? "" : "Nyomas = leallitas");
  } else if (tile == 3) {
    SignalStatus signal = recorderSignalStatus();
    char meter[25];
    snprintf(meter, sizeof(meter), "RMS %u  Csucs %u", signal.rms, signal.peak);
    oled.drawStr(0, 36, signal.enabled ? "Nyomas: meres KI" : "Nyomas: meres BE");
    oled.drawStr(0, 48, signal.active ? meter : signal.enabled ? "Meres indul..." : "Meres kikapcsolva");
  } else if (tile == 4) {
    char firmware[24];
    snprintf(firmware, sizeof(firmware), "FW: v%s", appFirmwareVersion().c_str());
    String buildTime = appFirmwareBuildDateTime();
    oled.drawStr(0, 32, appSdReady() ? "SD: OK" : "SD: hianyzik");
    oled.drawStr(0, 40, recorderAdcReady() ? "ADC: OK" : "ADC: hiba");
    oled.drawStr(0, 48, firmware);
    oled.drawStr(0, 56, buildTime.c_str());
  } else if (tile == 6) {
    oled.drawStr(0, 37, "Nyomas = beallitasok");
  } else {
    oled.drawStr(0, 38, tile == 5 ? "Nincs radio HW" : "Fajlkezelo kesobb");
  }
  oled.drawHLine(0, 56, 128);
  if (messagePending) {
    oled.drawStr(0, 63, message);
    messagePending = false;
  } else {
    char footer[21];
    snprintf(footer, sizeof(footer), inSettings ? "Hosszu: vissza" : "< %u/%u >  Hosszu: vissza", tile + 1, kTileCount);
    oled.drawStr(0, 63, footer);
  }
  if (fullUpdate) oled.sendBuffer();
  else oled.updateDisplayArea(0, 3, 16, 4);
  lastBodyAt = millis();
}

void onPress() {
  if (inSettings) {
    if (editing) { editing = false; return; }
    if (setting == 6) { inSettings = false; return; }
    if (setting == 0) { if (appWifiTestActive()) setMessage("Wi-Fi zajteszt aktiv"); else toggleWifi(); return; }
    if (setting == 1) { if (!recorderSetSerialMeter(!recorderSerialMeterEnabled())) setMessage("ADC nem elerheto"); return; }
    if (setting == 4) { recorderSetLowpass(!recorderLowpass()); return; }
    if (setting == 5) { editing = true; return; }
    editing = true;
  } else if (tile == 0) {
    if (appWifiTestActive()) setMessage("Zajteszt aktiv");
    else if (recorderState() == CaptureState::idle) {
      if (!appStartRecording()) setMessage("Felvetel nem indithato");
    } else if (!recorderStopping()) appStopRecording();
  } else if (tile == 2) {
    if (!playbackStop()) setMessage("Nincs lejatszas");
  } else if (tile == 3) {
    if (!recorderAdcReady() || !recorderTaskReady()) setMessage("ADC nem elerheto");
    else recorderSetSignal(!recorderSignalStatus().enabled);
  } else if (tile == 6) {
    inSettings = true;
    setting = 0;
  } else if (tile == 5) {
    setMessage("Radio HW nem elerheto");
  }
}
}

void initDisplay() {
#if RADIORECORDER_OLED_ENABLED
  if (!Wire.begin(kSdaGpio, kSclGpio)) {
    Serial.println("OLED: I2C initialization failed on SDA=GPIO4, SCL=GPIO5");
    return;
  }
  uint8_t address = 0;
  for (uint8_t candidate : {0x3c, 0x3d}) {
    Wire.beginTransmission(candidate);
    uint8_t result = Wire.endTransmission();
    Serial.printf("OLED: I2C address 0x%02X probe result %u\n", candidate, result);
    if (result == 0) { address = candidate; break; }
  }
  if (!address) {
    Serial.println("OLED: no SH1106 ACK at 0x3C or 0x3D");
    return;
  }
  Serial.printf("OLED: found at I2C address 0x%02X\n", address);
  oled.setI2CAddress(address << 1);
  oled.setBusClock(100000);
  oled.begin();
  ready = true;
  lastActivityAt = millis();
  oled.clearBuffer();
  oled.drawXBM(0, 0, 128, 64, kStartupImageBits);
  oled.sendBuffer();
#endif
}

void displayShowHome() {
  if (ready) drawBody();
}

bool displayReady() { return ready; }

void displaySleep() {
  if (!ready) return;
  oled.setPowerSave(1);
  screenSleeping = true;
}

void displayWake() {
  if (!ready) return;
  lastActivityAt = millis();
  if (screenSleeping) {
    oled.setPowerSave(0);
    screenSleeping = false;
    drawBody();
  }
}

void printDisplayState() {
  if (!ready) {
    Serial.println("OLED_ERROR unavailable");
    return;
  }
  Serial.printf("OLED_STATE %u %u %u %u %u\n", tile, setting, inSettings, editing, screenSleeping);
}

void printDisplayFrame() {
  if (!ready) {
    Serial.println("OLED_ERROR unavailable");
    return;
  }
  static const char digits[] = "0123456789ABCDEF";
  const uint8_t *buffer = oled.getBufferPtr();
  Serial.print("OLED_FRAME 128 64 ");
  for (size_t offset = 0; offset < 1024; offset += 32) {
    char hex[64];
    for (size_t index = 0; index < 32; ++index) {
      uint8_t value = buffer[offset + index];
      hex[index * 2] = digits[value >> 4];
      hex[index * 2 + 1] = digits[value & 15];
    }
    Serial.write(reinterpret_cast<const uint8_t *>(hex), sizeof(hex));
  }
  Serial.println();
}

void pollDisplay(int32_t steps, ButtonEvent button) {
  if (!ready) return;
  uint32_t now = millis();
  bool activity = steps != 0 || button != ButtonEvent::none;
  if (activity) lastActivityAt = now;
  if (screenSleeping) {
    if (!activity) return;
    oled.setPowerSave(0);
    screenSleeping = false;
    drawBody();
    return;
  }
  bool redraw = false;
  if (button == ButtonEvent::longPress) {
    if (editing) editing = false;
    else inSettings = false;
    redraw = true;
  } else if (button == ButtonEvent::shortPress) {
    onPress();
    redraw = true;
  }
  if (steps) {
    if (inSettings) {
      if (editing && setting == 2) {
        int32_t volume = playbackStatus().volume + steps * 5;
        playbackSetVolume(constrain(volume, 0L, 100L));
      } else if (editing && setting == 3) {
        uint8_t boost = playbackStatus().boost;
        while (steps > 0) { boost = boost == 1 ? 2 : 4; --steps; }
        while (steps < 0) { boost = boost == 4 ? 2 : 1; ++steps; }
        playbackSetBoost(boost);
      } else if (editing && setting == 5) {
        int32_t minutes = static_cast<int32_t>(appScreenOffMinutes()) + steps;
        minutes = constrain(minutes, 0L, 999L);
        if (!appSetScreenOffMinutes(static_cast<uint16_t>(minutes))) setMessage("NVS mentesi hiba");
      } else {
        setting = (setting + (steps % kSettingsCount) + kSettingsCount) % kSettingsCount;
      }
    } else {
      tile = (tile + (steps % kTileCount) + kTileCount) % kTileCount;
    }
    redraw = true;
  }
  uint32_t second = appDemoTimeSeconds();
  uint32_t elapsed = now - lastSpinnerAt;
  bool spinnerTick = elapsed >= 100;
  if (spinnerTick) {
    uint32_t frames = elapsed / 100;
    spinner = (spinner + frames) % 8;
    lastSpinnerAt += frames * 100;
  }
  if (redraw) {
    lastSecond = second;
    drawBody();
  } else if (!inSettings && (tile == 0 || tile == 3) && now - lastBodyAt >= 500) {
    bool secondChanged = second != lastSecond;
    lastSecond = second;
    drawBody(false);
    if (secondChanged) oled.updateDisplayArea(0, 0, 16, 2);
    else if (spinnerTick) oled.updateDisplayArea(0, 0, 1, 1);
  } else if (second != lastSecond) {
    lastSecond = second;
    if (inSettings || tile == 2 || tile == 4) {
      drawBody(false);
      oled.updateDisplayArea(0, 0, 16, 2);
    } else {
      drawStatus(second);
      oled.updateDisplayArea(0, 0, 16, 2);
    }
  } else if (spinnerTick) {
    drawStatus(second);
    oled.updateDisplayArea(0, 0, 1, 1);
  }
  uint16_t timeoutMinutes = appScreenOffMinutes();
  if (timeoutMinutes > 0 && now - lastActivityAt >= static_cast<uint32_t>(timeoutMinutes) * 60000UL) {
    displaySleep();
  }
}