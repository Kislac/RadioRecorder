#include "display.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "startup_image.h"
#include "app_control.h"
#include "playback.h"
#include "recorder.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "ptt_test.h"
#include "wav.h"

#ifndef RADIORECORDER_OLED_ENABLED
#define RADIORECORDER_OLED_ENABLED 0
#endif

namespace {
constexpr int kSdaGpio = 4;
constexpr int kSclGpio = 5;
constexpr uint8_t kTileCount = 12;
constexpr uint8_t kSettingsCount = 15;
constexpr uint8_t kBrowseRows = 5;
constexpr uint8_t kSettingsRows = 5;
constexpr uint8_t kFileTile = 4;
constexpr uint8_t kRadioSignalTile = 5;
constexpr uint8_t kMicSignalTile = 6;
constexpr uint8_t kDiagnosticsTile = 7;
constexpr uint8_t kPttTile = 8;
constexpr uint8_t kWifiTile = 9;
constexpr uint8_t kSettingsTile = 10;
constexpr uint8_t kTimeTile = 11;
const char *const kTileNames[kTileCount] = {"Man Radio Felvetel", "Man Mikrofon Felvetel", "Aut Radio Felvetel",
                                            "Aut Mikrofon Felvetel", "Fajlkezelo", "Radio Elo jel", "Mikrofon Elo jel",
                                            "Diagnosztika", "PTT aktivalas", "Wi-Fi", "Beallitasok", "Time"};

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
bool browsingFiles = false;
bool choosingPlayback = false;
bool graphMode = false;
bool ownsRadioSignal = false;
bool ownsMicSignal = false;
uint16_t graphSamples[120] = {};
constexpr uint8_t kGraphSamples = sizeof(graphSamples) / sizeof(graphSamples[0]);
constexpr uint32_t kSignalSettlingMs = 2000;
uint8_t graphCount = 0;
uint8_t graphNext = 0;
uint32_t lastGraphSampleAt = 0;
uint32_t signalActiveAt = 0;
uint16_t graphMaxRms = 0;
String browsePath = "/";
String selectedFile;
uint32_t browseSelected = 0;
uint32_t browseTotal = 0;
uint8_t playbackChoice = 0;
struct BrowseRow {
  String name;
  String path;
  bool directory = false;
};
BrowseRow browseRows[kBrowseRows];
tm clockEdit = {};
uint8_t zoneEdit = 0;
char message[26] = {};

void setMessage(const char *text) {
  snprintf(message, sizeof(message), "%s", text);
  messagePending = true;
}

bool isSignalTile(uint8_t selectedTile) {
  return selectedTile == kRadioSignalTile || selectedTile == kMicSignalTile;
}

SignalStatus selectedSignal() {
  return tile == kMicSignalTile ? recorderMicSignalStatus() : recorderSignalStatus();
}

void startDisplaySignal() {
  graphMode = false;
  graphCount = graphNext = 0;
  graphMaxRms = 0;
  signalActiveAt = 0;
  lastGraphSampleAt = millis();
  if (tile == kRadioSignalTile && !recorderSignalStatus().enabled && recorderAdcReady() && recorderTaskReady()) {
    recorderSetSignal(true);
    ownsRadioSignal = recorderSignalStatus().enabled;
  } else if (tile == kMicSignalTile && !recorderMicSignalStatus().enabled) {
    ownsMicSignal = recorderSetMicSignal(true);
  }
}

void stopDisplaySignal() {
  if (ownsRadioSignal) recorderSetSignal(false);
  if (ownsMicSignal) recorderSetMicSignal(false);
  ownsRadioSignal = ownsMicSignal = false;
  graphMode = false;
}

void sampleDisplaySignal(uint32_t now) {
  if (!isSignalTile(tile)) return;
  SignalStatus signal = selectedSignal();
  if (!signal.enabled || !signal.active) { signalActiveAt = 0; return; }
  if (!signalActiveAt) { signalActiveAt = now; lastGraphSampleAt = now; }
  if (static_cast<uint32_t>(now - signalActiveAt) < kSignalSettlingMs) return;
  if (signal.rms > graphMaxRms) graphMaxRms = signal.rms;
  if (static_cast<uint32_t>(now - lastGraphSampleAt) < 1000) return;
  lastGraphSampleAt = now;
  graphSamples[graphNext] = signal.rms;
  graphNext = (graphNext + 1) % kGraphSamples;
  if (graphCount < kGraphSamples) ++graphCount;
}

void drawSignal() {
  SignalStatus signal = selectedSignal();
  if (!signal.enabled) { oled.drawStr(0, 36, "Jelmeres nem elerheto"); return; }
  if (!signal.active) { oled.drawStr(0, 36, "Meres indul..."); return; }
  char meter[27];
  snprintf(meter, sizeof(meter), graphMode ? "RMS %u  Max %u" : "RMS %u  Csucs %u", signal.rms,
           graphMode ? graphMaxRms : signal.peak);
  oled.drawStr(0, 33, meter);
  if (!graphMode) {
    snprintf(meter, sizeof(meter), "Max RMS %u", graphMaxRms);
    oled.drawStr(0, 45, meter);
    oled.drawStr(0, 53, "Nyomas: grafikon");
    return;
  }
  oled.drawHLine(0, 62, 128);
  for (uint8_t index = 0; index < graphCount; ++index) {
    uint16_t sample = graphSamples[(graphNext + kGraphSamples - graphCount + index) % kGraphSamples];
    uint8_t height = graphMaxRms ? static_cast<uint32_t>(sample) * 24 / graphMaxRms : 0;
    oled.drawVLine(8 + index, 61 - height, height + 1);
  }
}

void drawRecordingName(const char *path) {
  String name(path);
  if (name.length() <= 24) { oled.drawStr(0, 46, name.c_str()); return; }
  oled.drawStr(0, 44, name.substring(0, 24).c_str());
  oled.drawStr(0, 53, name.substring(24, 48).c_str());
}

void drawBuildDate() {
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char month[4] = {};
  int day = 0;
  int year = 0;
  sscanf(__DATE__, "%3s %d %d", month, &day, &year);
  uint8_t monthNumber = 0;
  for (uint8_t index = 0; index < 12; ++index) {
    if (strcmp(month, months[index]) == 0) monthNumber = index + 1;
  }
  char date[24];
  snprintf(date, sizeof(date), "%04d.%02u.%02d %s", year, monthNumber, day, __TIME__);
  oled.drawStr(0, 55, date);
}

void drawWifi() {
  oled.setFont(u8g2_font_5x8_tf);
  String mode = wifiManagerModeName();
  oled.drawStr(128 - oled.getStrWidth(mode.c_str()), 22, mode.c_str());
  bool station = WiFi.status() == WL_CONNECTED;
  wifi_mode_t wifiMode = WiFi.getMode();
  bool ap = wifiMode == WIFI_MODE_AP || wifiMode == WIFI_MODE_APSTA;
  String ssid = station ? WiFi.SSID() : mode == "CONNECTING" ? wifiManagerSSID() : "-";
  oled.drawStr(0, 30, "STA SSID:");
  if (ssid.length() > 15) {
    uint32_t offset = (millis() / 400) % (ssid.length() + 4);
    ssid = (ssid + "    " + ssid).substring(offset, offset + 15);
  }
  oled.drawStr(48, 30, ssid.c_str());
  String staIp = station ? WiFi.localIP().toString() : "-";
  oled.drawStr(0, 38, "STA IP:");
  oled.drawStr(40, 38, staIp.c_str());
  oled.drawStr(0, 46, "AP SSID:");
  oled.drawStr(45, 46, ap ? WiFi.softAPSSID().c_str() : "-");
  String apIp = ap ? WiFi.softAPIP().toString() : "-";
  oled.drawStr(0, 54, "AP IP:");
  oled.drawStr(40, 54, apIp.c_str());
}

void refreshBrowseRows() {
  for (auto &row : browseRows) row = {};
  browseTotal = 0;
  if (!appSdReady() || recorderState() != CaptureState::idle) return;
  File directory = SD_MMC.open(browsePath, FILE_READ);
  if (!directory || !directory.isDirectory()) return;
  bool hasParent = browsePath != "/";
  browseTotal = hasParent ? 1 : 0;
  File entry;
  while ((entry = directory.openNextFile())) {
    String name = entry.name();
    name.toLowerCase();
    if (entry.isDirectory() || name.endsWith(".wav")) ++browseTotal;
    entry.close();
  }
  directory.close();
  if (browseTotal && browseSelected >= browseTotal) browseSelected = browseTotal - 1;
  uint32_t first = (browseSelected / kBrowseRows) * kBrowseRows;
  if (hasParent && first == 0) {
    int separator = browsePath.lastIndexOf('/');
    browseRows[0] = {"..", separator == 0 ? "/" : browsePath.substring(0, separator), true};
  }
  uint32_t index = hasParent ? 1 : 0;
  for (uint8_t pass = 0; pass < 2 && index < first + kBrowseRows; ++pass) {
    directory = SD_MMC.open(browsePath, FILE_READ);
    if (!directory) break;
    while ((entry = directory.openNextFile())) {
      bool folder = entry.isDirectory();
      String name = entry.name();
      name = name.substring(name.lastIndexOf('/') + 1);
      String lower = name;
      lower.toLowerCase();
      if (folder == (pass == 0) && (folder || lower.endsWith(".wav"))) {
        if (index >= first && index < first + kBrowseRows) {
          String path = browsePath == "/" ? "/" + name : browsePath + "/" + name;
          browseRows[index - first] = {name, path, folder};
        }
        ++index;
      }
      entry.close();
      if (index >= first + kBrowseRows) break;
    }
    directory.close();
  }
}

String shortBrowseName(const String &name) {
  if (name.length() <= 22) return name;
  return name.substring(0, 8) + ".." + name.substring(name.length() - 12);
}

void browseParent() {
  if (browsePath == "/") { browsingFiles = false; return; }
  int separator = browsePath.lastIndexOf('/');
  browsePath = separator == 0 ? "/" : browsePath.substring(0, separator);
  browseSelected = 0;
  refreshBrowseRows();
}

void pressBrowse() {
  if (choosingPlayback) {
    if (playbackChoice == 2) { choosingPlayback = false; return; }
    PlaybackTarget target = playbackChoice == 1 ? PlaybackTarget::radio : PlaybackTarget::speaker;
    PlaybackStatus status = playbackStatus();
    if (!appSdReady() || (target == PlaybackTarget::radio ? !status.radioReady : !status.ready)) {
      setMessage("SD/hangkimenet hianyzik"); return;
    }
    if (!appCanStartPlayback()) { setMessage("Lejatszas nem indithato"); return; }
    if (selectedFile.length() > 220) { setMessage("Tul hosszu fajlnev"); return; }
    File file = SD_MMC.open(selectedFile, FILE_READ);
    uint32_t remaining = 0;
    bool supported = file && !file.isDirectory() && findPcmData(file, remaining);
    if (file) file.close();
    if (!supported) { setMessage("Nem tamogatott WAV"); return; }
    if (target == PlaybackTarget::radio) playbackStart(selectedFile, PlaybackTarget::radio);
    else playbackStart(selectedFile);
    choosingPlayback = false;
    browsingFiles = false;
    return;
  }
  refreshBrowseRows();
  if (!appSdReady()) { setMessage("SD nem elerheto"); return; }
  if (recorderState() != CaptureState::idle) { setMessage("Felvetel folyamatban"); return; }
  if (!browseTotal) return;
  const BrowseRow &item = browseRows[browseSelected % kBrowseRows];
  if (item.path.isEmpty()) return;
  if (item.directory) {
    if (item.name == "..") { browseParent(); return; }
    if (item.path.length() > 220) { setMessage("Tul hosszu utvonal"); return; }
    browsePath = item.path;
    browseSelected = 0;
    refreshBrowseRows();
  } else {
    selectedFile = item.path;
    playbackChoice = 0;
    choosingPlayback = true;
  }
}

void drawBrowse() {
  oled.setFont(u8g2_font_5x8_tf);
  if (choosingPlayback) {
    oled.drawStr(0, 30, shortBrowseName(selectedFile.substring(selectedFile.lastIndexOf('/') + 1)).c_str());
    oled.drawStr(0, 38, playbackChoice == 0 ? "> Hangszoro" : "  Hangszoro");
    oled.drawStr(0, 46, playbackChoice == 1 ? "> Radio DAC" : "  Radio DAC");
    oled.drawStr(0, 54, playbackChoice == 2 ? "> Vissza" : "  Vissza");
    return;
  }
  if (!browsingFiles) {
    PlaybackStatus playback = playbackStatus();
    oled.drawStr(0, 35, playback.state == PlaybackState::idle ? "Nyomas: SD fajlok" : "Lejatszas folyamatban");
    oled.drawStr(0, 48, playback.state == PlaybackState::idle ? "" : "Nyomas = leallitas");
    return;
  }
  if (!appSdReady()) { oled.drawStr(0, 36, "SD nem elerheto"); return; }
  if (recorderState() != CaptureState::idle) { oled.drawStr(0, 36, "Felvetel folyamatban"); return; }
  if (!browseTotal) { oled.drawStr(0, 36, "Nincs WAV vagy mappa"); return; }
  uint32_t first = (browseSelected / kBrowseRows) * kBrowseRows;
  for (uint8_t row = 0; row < kBrowseRows && first + row < browseTotal; ++row) {
    const BrowseRow &item = browseRows[row];
    uint8_t y = 30 + row * 8;
    oled.drawStr(0, y, first + row == browseSelected ? ">" : " ");
    String label = shortBrowseName(item.name);
    if (item.directory) label += "/";
    oled.drawStr(9, y, label.c_str());
  }
}

void drawStatus() {
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
  tm local = {};
  char clockText[24];
  if (!appClockLocal(local)) snprintf(clockText, sizeof(clockText), "------ --:--:--");
  else strftime(clockText, sizeof(clockText), "%y%m%d %H:%M:%S", &local);
  oled.drawStr(128 - oled.getStrWidth(clockText), 8, clockText);
  oled.drawHLine(0, 10, 128);
}

void drawTime() {
  tm local = {};
  char dateTime[24];
  if (appClockLocal(local)) strftime(dateTime, sizeof(dateTime), "%Y.%m.%d %H:%M:%S", &local);
  else snprintf(dateTime, sizeof(dateTime), "Time unknown");
  oled.drawStr(0, 31, dateTime);
  char source[26];
  snprintf(source, sizeof(source), "Source: %s", appClockSourceLabel());
  oled.drawStr(0, 39, source);
  oled.drawStr(0, 47, "Runtime");
  oled.drawStr(0, 55, appUptimeText().c_str());
}

void drawSettingValue(uint8_t y, const char *value) {
  oled.drawStr(127 - oled.getStrWidth(value), y, value);
}

void drawSettings() {
  oled.setFont(u8g2_font_5x8_tf);
  tm local = {};
  if (!appClockLocal(local)) { local.tm_year = 126; local.tm_mday = 1; }
  for (uint8_t index = 0; index < kSettingsCount; ++index) {
    uint8_t row = (setting / kSettingsRows) * kSettingsRows;
    if (index < row || index >= row + kSettingsRows) continue;
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
      case 6: oled.drawStr(9, y, "Wi-Fi ido"); {
        char value[10];
        uint16_t minutes = wifiManagerAutoOffMinutes();
        if (minutes == 0) snprintf(value, sizeof(value), "0 min");
        else snprintf(value, sizeof(value), "%u min", minutes);
        drawSettingValue(y, value);
        break;
      }
      case 7: oled.drawStr(9, y, "Ev"); {
        char value[16];
        snprintf(value, sizeof(value), "%d", (editing && setting == index ? clockEdit : local).tm_year + 1900);
        drawSettingValue(y, value);
        break;
      }
      case 8: oled.drawStr(9, y, "Honap"); {
        char value[16];
        snprintf(value, sizeof(value), "%02d", (editing && setting == index ? clockEdit : local).tm_mon + 1);
        drawSettingValue(y, value);
        break;
      }
      case 9: oled.drawStr(9, y, "Nap"); {
        char value[16];
        snprintf(value, sizeof(value), "%02d", (editing && setting == index ? clockEdit : local).tm_mday);
        drawSettingValue(y, value);
        break;
      }
      case 10: oled.drawStr(9, y, "Ora"); {
        char value[16];
        snprintf(value, sizeof(value), "%02d", (editing && setting == index ? clockEdit : local).tm_hour);
        drawSettingValue(y, value);
        break;
      }
      case 11: oled.drawStr(9, y, "Perc"); {
        char value[16];
        snprintf(value, sizeof(value), "%02d", (editing && setting == index ? clockEdit : local).tm_min);
        drawSettingValue(y, value);
        break;
      }
      case 12: oled.drawStr(9, y, "Idozona"); {
        uint8_t zone = editing && setting == index ? zoneEdit : appClockTimezone();
        char value[12];
        if (zone == 0) snprintf(value, sizeof(value), "Budapest");
        else if (zone == 1) snprintf(value, sizeof(value), "UTC");
        else snprintf(value, sizeof(value), "UTC%+d", static_cast<int>(zone) - 14);
        drawSettingValue(y, value);
        break;
      }
      case 13: oled.drawStr(9, y, "Radio hang"); {
        char value[5];
        snprintf(value, sizeof(value), "%u%%", playbackStatus().radioVolume);
        drawSettingValue(y, value);
        break;
      }
      default: oled.drawStr(9, y, "Vissza"); break;
    }
  }
}

void drawBody(bool fullUpdate = true) {
  oled.clearBuffer();
  drawStatus();
  oled.setFont(u8g2_font_6x10_tf);
  String heading = inSettings ? "Beallitasok" : tile == kFileTile && browsingFiles ?
                   (choosingPlayback ? "Lejatszas" : browsePath == "/" ? "microSD" :
                    browsePath.substring(browsePath.lastIndexOf('/') + 1, browsePath.lastIndexOf('/') + 19)) : kTileNames[tile];
  oled.drawStr(0, 22, heading.c_str());
  oled.setFont(u8g2_font_5x8_tf);
  if (inSettings) {
    drawSettings();
  } else if (tile == 0) {
    CaptureState state = recorderState();
    bool adcRecording = state != CaptureState::idle && recorderSource() == CaptureSource::adc;
    oled.drawStr(0, 34, state == CaptureState::idle ? "ADC: nyomas = indit" :
                 !adcRecording ? "MIC felvetel aktiv" : recorderStopping() ? "Lezaras..." :
                 state == CaptureState::starting ? "Indulas..." : "ADC REC: nyomas = stop");
    if (adcRecording) drawRecordingName(recorderName());
  } else if (tile == kFileTile) {
    drawBrowse();
  } else if (isSignalTile(tile)) {
    drawSignal();
  } else if (tile == kDiagnosticsTile) {
    char firmware[24];
    snprintf(firmware, sizeof(firmware), "FW: v%s", appFirmwareVersion().c_str());
    oled.drawStr(0, 32, strcmp(appRtcStatus(), "ready") == 0 ? "RTC: OK" :
                 strcmp(appRtcStatus(), "unavailable") == 0 ? "RTC: hianyzik" : "RTC: hibas ido");
    oled.drawStr(0, 40, appSdReady() ? (recorderAdcReady() ? "SD: OK   ADC: OK" : "SD: OK   ADC: hiba") :
                 (recorderAdcReady() ? "SD: nincs ADC: OK" : "SD: nincs ADC: hiba"));
    oled.drawStr(0, 48, firmware);
    drawBuildDate();
  } else if (tile == kWifiTile) {
    drawWifi();
  } else if (tile == kTimeTile) {
    drawTime();
  } else if (tile == kSettingsTile) {
    oled.drawStr(0, 37, "Nyomas = beallitasok");
  } else if (tile == 1) {
    CaptureState state = recorderState();
    bool micRecording = state != CaptureState::idle && recorderSource() == CaptureSource::microphone;
    oled.drawStr(0, 35, !recorderMicReady() ? "I2S nem elerheto" :
                 micRecording ? (recorderStopping() ? "Lezaras..." : "MIC REC: nyomas = stop") :
                 state != CaptureState::idle ? "Masik felvetel aktiv" : "Nyomas = MIC indit");
    if (micRecording) drawRecordingName(recorderName());
  } else if (tile == kPttTile) {
    oled.drawStr(0, 36, "PTT teszt / GPIO10");
    oled.drawStr(0, 48, pttTestActive() ? "AKTIV - nyomas: KI" : "Nyomas = BE");
  } else if (tile == 2) {
    oled.drawStr(0, 38, "Auto radio: kesobb");
  } else if (tile == 3) {
    oled.drawStr(0, 38, "Auto mikrofon: kesobb");
  } else {
    oled.drawStr(0, 38, "Nem elerheto");
  }
  bool inMenu = inSettings || (tile == kFileTile && browsingFiles) || (isSignalTile(tile) && graphMode);
  if (!inMenu) oled.drawHLine(0, 56, 128);
  if (messagePending) {
    if (inMenu) { oled.setDrawColor(0); oled.drawBox(0, 55, 128, 9); oled.setDrawColor(1); }
    oled.drawStr(0, 63, message);
    messagePending = false;
  } else if (!inMenu) {
    char footer[12];
    snprintf(footer, sizeof(footer), "< %u/%u >", tile + 1, kTileCount);
    oled.drawStr(0, 63, footer);
  }
  if (fullUpdate) oled.sendBuffer();
  else oled.updateDisplayArea(0, 2, 16, 6);
  lastBodyAt = millis();
}

void onPress() {
  if (inSettings) {
    if (editing) {
      if (setting == 12) {
        if (!appClockSetTimezone(zoneEdit)) setMessage("Idozona mentese hiba");
      } else if (setting >= 7 && setting <= 11) {
        char value[64];
        snprintf(value, sizeof(value), "%04d-%02d-%02dT%02d:%02d", clockEdit.tm_year + 1900,
                 clockEdit.tm_mon + 1, clockEdit.tm_mday, clockEdit.tm_hour, clockEdit.tm_min);
        if (!appClockSetLocal(value)) setMessage("Ervenytelen ido/NVS");
      }
      editing = false;
      return;
    }
    if (setting == 14) { inSettings = false; return; }
    if (setting == 0) { if (!wifiManagerToggle()) setMessage("Wi-Fi valtas sikertelen"); return; }
    if (setting == 1) { if (!recorderSetSerialMeter(!recorderSerialMeterEnabled())) setMessage("ADC nem elerheto"); return; }
    if (setting == 4) { recorderSetLowpass(!recorderLowpass()); return; }
    if (setting == 5) { editing = true; return; }
    if (setting == 6) { editing = true; return; }
    if (setting >= 7 && setting <= 11) {
      clockEdit = {};
      if (!appClockLocal(clockEdit)) { clockEdit.tm_year = 126; clockEdit.tm_mday = 1; }
    }
    if (setting == 12) zoneEdit = appClockTimezone();
    editing = true;
  } else if (tile == 0) {
    if (recorderState() == CaptureState::idle) {
      if (!appStartRecording()) setMessage("Felvetel nem indithato");
    } else if (recorderSource() == CaptureSource::adc && !recorderStopping()) appStopRecording();
  } else if (tile == kFileTile) {
    if (browsingFiles) pressBrowse();
    else if (playbackGetState() != PlaybackState::idle) playbackStop();
    else {
      browsingFiles = true;
      browsePath = "/";
      browseSelected = 0;
      refreshBrowseRows();
    }
  } else if (tile == 1) {
    if (recorderState() == CaptureState::idle) {
      if (!appStartMicRecording()) setMessage("MIC nem indithato");
    } else if (recorderSource() == CaptureSource::microphone && !recorderStopping()) appStopRecording();
  } else if (isSignalTile(tile)) {
    if (!selectedSignal().enabled) setMessage("Jelmeres nem elerheto");
    else graphMode = !graphMode;
  } else if (tile == kSettingsTile) {
    inSettings = true;
    setting = 0;
  } else if (tile == kWifiTile) {
    if (wifiManagerHasCredentials()) {
      if (!wifiManagerForget()) setMessage("Wi-Fi elfelejtes sikertelen");
    } else {
      setMessage("Nincs mentett Wi-Fi");
    }
  } else if (tile == kPttTile) {
    if (pttTestActive()) stopPttTest();
    else if (!latchPttTest()) setMessage("PTT nem indithato");
  }
}
}

void initDisplay() {
#if RADIORECORDER_OLED_ENABLED
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
  if (isSignalTile(tile)) stopDisplaySignal();
  oled.setPowerSave(1);
  screenSleeping = true;
}

void displayWake() {
  if (!ready) return;
  lastActivityAt = millis();
  if (screenSleeping) {
    oled.setPowerSave(0);
    screenSleeping = false;
    if (isSignalTile(tile)) startDisplaySignal();
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
    if (isSignalTile(tile)) startDisplaySignal();
    drawBody();
    return;
  }
  bool redraw = false;
  if (button == ButtonEvent::longPress) {
    if (editing) editing = false;
    else if (choosingPlayback) choosingPlayback = false;
    else if (browsingFiles) browseParent();
    else inSettings = false;
    redraw = true;
  } else if (button == ButtonEvent::shortPress) {
    onPress();
    redraw = true;
  }
  if (steps) {
    if (choosingPlayback) {
      playbackChoice = (playbackChoice + (steps % 3) + 3) % 3;
    } else if (browsingFiles) {
      refreshBrowseRows();
      if (browseTotal) {
        int64_t next = static_cast<int64_t>(browseSelected) + steps;
        browseSelected = static_cast<uint32_t>(constrain(next, 0LL, static_cast<int64_t>(browseTotal - 1)));
        refreshBrowseRows();
      }
    } else if (inSettings) {
      if (editing && setting == 2) {
        int32_t volume = playbackStatus().volume + steps * 5;
        playbackSetVolume(constrain(volume, 0L, 100L));
      } else if (editing && setting == 13) {
        int32_t volume = playbackStatus().radioVolume + steps * 5;
        playbackSetRadioVolume(constrain(volume, 0L, 100L));
      } else if (editing && setting == 3) {
        uint8_t boost = playbackStatus().boost;
        while (steps > 0) { boost = boost == 1 ? 2 : 4; --steps; }
        while (steps < 0) { boost = boost == 4 ? 2 : 1; ++steps; }
        playbackSetBoost(boost);
      } else if (editing && setting == 5) {
        int32_t minutes = static_cast<int32_t>(appScreenOffMinutes()) + steps;
        minutes = constrain(minutes, 0L, 999L);
        if (!appSetScreenOffMinutes(static_cast<uint16_t>(minutes))) setMessage("NVS mentesi hiba");
      } else if (editing && setting == 6) {
        int32_t minutes = static_cast<int32_t>(wifiManagerAutoOffMinutes()) + steps;
        minutes = constrain(minutes, 0L, 999L);
        if (!wifiManagerSetAutoOffMinutes(static_cast<uint16_t>(minutes))) setMessage("NVS mentesi hiba");
      } else if (editing && setting >= 7 && setting <= 11) {
        int *field = setting == 7 ? &clockEdit.tm_year : setting == 8 ? &clockEdit.tm_mon :
                         setting == 9 ? &clockEdit.tm_mday : setting == 10 ? &clockEdit.tm_hour : &clockEdit.tm_min;
        int32_t minimum = setting == 7 ? 120 : setting == 9 ? 1 : 0;
        int32_t maximum = setting == 7 ? 199 : setting == 8 ? 11 : setting == 9 ? 31 : setting == 10 ? 23 : 59;
        *field = constrain(*field + steps, minimum, maximum);
      } else if (editing && setting == 12) {
        zoneEdit = constrain(static_cast<int32_t>(zoneEdit) + steps, 0L, 28L);
      } else {
        setting = (setting + (steps % kSettingsCount) + kSettingsCount) % kSettingsCount;
      }
    } else {
      uint8_t previousTile = tile;
      tile = (tile + (steps % kTileCount) + kTileCount) % kTileCount;
      if (tile != previousTile) {
        if (isSignalTile(previousTile)) stopDisplaySignal();
        if (isSignalTile(tile)) startDisplaySignal();
      }
    }
    redraw = true;
  }
  sampleDisplaySignal(now);
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
  } else if (!inSettings && (tile == 0 || tile == 1 || isSignalTile(tile) ||
                              tile == kPttTile || tile == kWifiTile || tile == kTimeTile) && now - lastBodyAt >= 500) {
    bool secondChanged = second != lastSecond;
    lastSecond = second;
    drawBody(false);
    if (secondChanged) oled.updateDisplayArea(0, 0, 16, 2);
    else if (spinnerTick) oled.updateDisplayArea(0, 0, 1, 1);
  } else if (second != lastSecond) {
    lastSecond = second;
    if (inSettings || tile == kFileTile || tile == kDiagnosticsTile) {
      drawBody(false);
      oled.updateDisplayArea(0, 0, 16, 2);
    } else {
      drawStatus();
      oled.updateDisplayArea(0, 0, 16, 2);
    }
  } else if (spinnerTick) {
    drawStatus();
    oled.updateDisplayArea(0, 0, 1, 1);
  }
  uint16_t timeoutMinutes = appScreenOffMinutes();
  if (timeoutMinutes > 0 && now - lastActivityAt >= static_cast<uint32_t>(timeoutMinutes) * 60000UL) {
    displaySleep();
  }
}