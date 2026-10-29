#include "radio_programming.h"
#include "ptt_test.h"
#include <HardwareSerial.h>
#include <cJSON.h>
#include <driver/gpio.h>

namespace {
HardwareSerial radioSerial(1);
enum class Phase { idle, magic, magicAck, ident, identAck, block, pause, retry, writeAck, writePause, done, failed };
enum class WriteStage { none, preflight, writing, verifying };
Phase phase = Phase::idle;
WriteStage writeStage = WriteStage::none;
uint8_t image[0x1948] = {};
uint8_t baselineImage[sizeof(image)] = {};
uint8_t desiredImage[sizeof(image)] = {};
size_t writeLength = 0;
uint16_t writeCursor = 8;
uint16_t bytesWritten = 0;
uint16_t writeBytes = 0;
bool writeVerified = false;
uint8_t reply[80] = {};
size_t received = 0;
size_t identLength = 0;
uint8_t magicIndex = 0;
uint8_t magicByte = 0;
uint16_t address = 0;
uint8_t blockSize = 64;
uint8_t probe = 0;
bool firstBlock = true;
bool droppedByte = false;
bool auxiliary = true;
uint32_t deadline = 0;
uint32_t startedAt = 0;
uint16_t completed = 0;
const char *failure = "";
const uint8_t magic[2][7] = {{0x50, 0xBB, 0xFF, 0x20, 0x12, 0x07, 0x25},
                            {0x50, 0xBB, 0xFF, 0x01, 0x25, 0x98, 0x4D}};

bool expired(uint32_t now) { return static_cast<int32_t>(now - deadline) >= 0; }

void releaseUart() {
  radioSerial.end();
  gpio_reset_pin(GPIO_NUM_17);
  gpio_reset_pin(GPIO_NUM_16);
}

void fail(const char *message) {
  failure = message;
  phase = Phase::failed;
  releaseUart();
}

void requestBlock(uint16_t nextAddress, uint8_t size) {
  address = nextAddress;
  blockSize = size;
  received = 0;
  uint8_t request[] = {0x53, static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address), size};
  radioSerial.write(request, sizeof(request));
  phase = Phase::block;
  deadline = millis() + 1200;
}

void beginAttempt() {
  radioSerial.begin(9600, SERIAL_8N1, 16, 17);
  while (radioSerial.available()) radioSerial.read();
  magicByte = 0;
  received = 0;
  identLength = 0;
  firstBlock = true;
  probe = 0;
  phase = Phase::magic;
  deadline = millis();
}

void nextWriteBlock() {
  while (writeCursor < 0x1808 && !memcmp(baselineImage + writeCursor, desiredImage + writeCursor, 16)) writeCursor += 16;
  if (writeCursor == 0x1808) {
    writeStage = WriteStage::verifying;
    completed = 0;
    probe = 4;
    requestBlock(0, 64);
    return;
  }
  uint16_t target = writeCursor - 8;
  uint8_t packet[20] = {0x58, static_cast<uint8_t>(target >> 8), static_cast<uint8_t>(target), 16};
  memcpy(packet + 4, desiredImage + writeCursor, 16);
  received = 0;
  radioSerial.write(packet, sizeof(packet));
  phase = Phase::writeAck;
  deadline = millis() + 1200;
}

void finishRead() {
  if (writeStage == WriteStage::preflight) {
    size_t length = auxiliary ? sizeof(image) : 0x1808;
    if (length != writeLength || memcmp(image, baselineImage, length)) {
      fail("Radio changed since backup; no writes sent. Read again");
      return;
    }
    while (radioSerial.available()) radioSerial.read();
    writeStage = WriteStage::writing;
    writeCursor = 8;
    nextWriteBlock();
    return;
  }
  if (writeStage == WriteStage::verifying) {
    if (memcmp(image, desiredImage, writeLength)) {
      fail("Write verification failed; radio may be partially changed. Keep the backup");
      return;
    }
    writeVerified = true;
  }
  phase = Phase::done;
  releaseUart();
}

void addNumber(cJSON *object, const char *name, int value) { cJSON_AddNumberToObject(object, name, value); }

bool frequency(const uint8_t *bytes, uint32_t &hz) {
  hz = 0;
  uint32_t multiplier = 10;
  for (uint8_t index = 0; index < 4; ++index) {
    uint8_t low = bytes[index] & 15;
    uint8_t high = bytes[index] >> 4;
    if (low > 9 || high > 9) return false;
    hz += (low + high * 10) * multiplier;
    multiplier *= 100;
  }
  return true;
}

void addFrequency(cJSON *object, const char *name, const uint8_t *bytes) {
  uint32_t hz = 0;
  if (frequency(bytes, hz)) cJSON_AddNumberToObject(object, name, hz);
  else cJSON_AddNullToObject(object, name);
}

void addVfo(cJSON *object, const char *name, size_t offset) {
  uint32_t units = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    if (image[offset + index] > 9) { cJSON_AddNullToObject(object, name); return; }
    units = units * 10 + image[offset + index];
  }
  cJSON_AddNumberToObject(object, name, units * 10UL);
}

void addText(cJSON *object, const char *name, size_t offset, size_t length) {
  char text[17] = {};
  if (length >= sizeof(text)) return;
  for (size_t index = 0; index < length; ++index) {
    uint8_t value = image[offset + index];
    text[index] = value >= 32 && value < 127 ? value : ' ';
  }
  while (length && text[length - 1] == ' ') text[--length] = '\0';
  cJSON_AddStringToObject(object, name, text);
}

void addVfoDetails(cJSON *root, const char *name, size_t offset) {
  cJSON *vfo = cJSON_AddObjectToObject(root, name);
  addVfo(vfo, "frequencyHz", offset);
  uint32_t shift = 0;
  bool valid = true;
  for (uint8_t index = 0; index < 6; ++index) {
    uint8_t digit = image[offset + 8 + index];
    if (digit > 9) valid = false;
    shift = shift * 10 + digit;
  }
  if (valid) cJSON_AddNumberToObject(vfo, "offsetHz", shift * 1000UL);
  else cJSON_AddNullToObject(vfo, "offsetHz");
  addNumber(vfo, "rxToneRaw", image[offset + 14] | image[offset + 15] << 8);
  addNumber(vfo, "txToneRaw", image[offset + 16] | image[offset + 17] << 8);
  addNumber(vfo, "bandRaw", image[offset + 18] & 1);
  addNumber(vfo, "shiftRaw", (image[offset + 20] >> 4) & 3);
  addNumber(vfo, "signalCodeRaw", image[offset + 20] & 15);
  addNumber(vfo, "stepRaw", (image[offset + 22] >> 4) & 7);
  addNumber(vfo, "powerRaw", image[offset + 23] >> 7);
  addNumber(vfo, "bandwidthRaw", (image[offset + 23] >> 6) & 1);
}

void addDtmfCode(cJSON *object, const char *name, size_t offset, size_t length) {
  const char alphabet[] = "0123456789 *#ABCD";
  char code[6] = {};
  size_t used = 0;
  for (size_t index = 0; index < length; ++index) {
    uint8_t value = image[offset + index];
    if (value == 0xFF || value == 0x1F) continue;
    code[used++] = value < sizeof(alphabet) - 1 ? alphabet[value] : '?';
  }
  cJSON_AddStringToObject(object, name, code);
}
}

bool radioProgrammingBusy() {
  return phase != Phase::idle && phase != Phase::done && phase != Phase::failed;
}

bool radioProgrammingStart() {
  if (radioProgrammingBusy() || pttTestActive()) return false;
  writeStage = WriteStage::none;
  writeVerified = false;
  bytesWritten = writeBytes = 0;
  memset(image, 0, sizeof(image));
  failure = "";
  completed = 0;
  magicIndex = 0;
  droppedByte = false;
  startedAt = millis();
  beginAttempt();
  return true;
}

String radioProgrammingRevision() {
  size_t length = 0;
  const uint8_t *data = radioProgrammingImage(length);
  if (!data) return "";
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < length; ++index) hash = (hash ^ data[index]) * 16777619UL;
  char text[9];
  snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(hash));
  return text;
}

bool radioProgrammingCanWrite() {
  return phase == Phase::done && auxiliary && !memcmp(image + 0x1838, "N5R2407BFB297", 13);
}

bool radioProgrammingWriteImage(const uint8_t *desired, size_t length, const char *revision, String &error) {
  if (!radioProgrammingCanWrite() || pttTestActive()) { error = "Read supported radio first; radio/PTT may be busy"; return false; }
  if (!revision || radioProgrammingRevision() != revision) { error = "Stale radio revision; read again"; return false; }
  if (!desired || length != sizeof(image) || memcmp(desired, image, 8) ||
      memcmp(desired + 0x1808, image + 0x1808, sizeof(image) - 0x1808) ||
      memcmp(desired + 0xCF8, image + 0xCF8, 16) || memcmp(desired + 0xDF8, image + 0xDF8, 16)) {
    error = "Protected image region or incompatible image";
    return false;
  }
  uint16_t changed = 0;
  for (size_t offset = 8; offset < 0x1808; offset += 16) if (memcmp(desired + offset, image + offset, 16)) changed += 16;
  if (!changed) { error = "No changes"; return false; }
  memcpy(baselineImage, image, length);
  memcpy(desiredImage, desired, length);
  writeLength = length;
  if (!radioProgrammingStart()) { error = "Radio busy"; return false; }
  writeStage = WriteStage::preflight;
  writeBytes = changed;
  return true;
}

void radioProgrammingPoll() {
  if (!radioProgrammingBusy()) return;
  uint32_t now = millis();
  if (now - startedAt > (writeStage == WriteStage::none ? 45000UL : 120000UL)) {
    fail("Radio operation timed out; keep the backup and power-cycle before retrying"); return;
  }
  if (phase == Phase::retry) {
    if (expired(now)) beginAttempt();
    return;
  }
  if (phase == Phase::magic) {
    if (!expired(now)) return;
    radioSerial.write(magic[magicIndex][magicByte++]);
    deadline = now + 10;
    if (magicByte == 7) { phase = Phase::magicAck; deadline = now + 1200; }
    return;
  }
  if (phase == Phase::writePause) {
    if (expired(now)) nextWriteBlock();
    return;
  }
  if (phase == Phase::pause) {
    if (!expired(now)) return;
    if (probe == 1) { requestBlock(0x1EC0, 64); return; }
    if (probe == 2) { requestBlock(0x1FC0, 64); return; }
    if (probe == 3) { probe = 4; requestBlock(0, 64); return; }
    if (address < 0x1800 && address + blockSize < 0x1800) { requestBlock(address + blockSize, 64); return; }
    if (address < 0x1800 && auxiliary) { requestBlock(0x1EC0, 64); return; }
    if (address < 0x1800) { finishRead(); return; }
    if (address + blockSize < 0x2000) {
      uint16_t next = address + blockSize;
      requestBlock(next, droppedByte && next >= 0x1FC0 ? 16 : 64);
      return;
    }
    finishRead();
    return;
  }
  while (radioSerial.available() && received < sizeof(reply)) reply[received++] = radioSerial.read();
  if (phase == Phase::writeAck) {
    if (received == 1 && reply[0] == 0x06) {
      bytesWritten += 16;
      writeCursor += 16;
      phase = Phase::writePause;
      deadline = now + 50;
    } else if (received || expired(now)) {
      fail("Write ACK missing or invalid; radio may be partially changed. Keep the backup");
    }
    return;
  }
  if (phase == Phase::magicAck) {
    if (received && reply[0] == 0x06) {
      radioSerial.write(static_cast<uint8_t>(0x02));
      received = 0;
      phase = Phase::ident;
      deadline = now + 1200;
    } else if (received || expired(now)) {
      releaseUart();
      if (magicIndex++ == 0) { phase = Phase::retry; deadline = now + 2000; }
      else fail("No radio ACK; check power, wiring, levels and model");
    }
    return;
  }
  if (phase == Phase::ident) {
    if (received && reply[received - 1] == 0xDD) {
      if (received != 8 && received != 12) { fail("Unsupported radio identification"); return; }
      identLength = received;
      if (identLength == 8) memcpy(image, reply, 8);
      else {
        image[0] = reply[0]; image[1] = reply[3]; image[2] = reply[5];
        memcpy(image + 3, reply + 7, 5);
      }
      radioSerial.write(static_cast<uint8_t>(0x06));
      received = 0;
      phase = Phase::identAck;
      deadline = now + 1200;
    } else if (received >= 12 || expired(now)) fail("Invalid or incomplete radio identification");
    return;
  }
  if (phase == Phase::identAck) {
    if (received == 1 && reply[0] == 0x06) {
      auxiliary = magicIndex == 0;
      probe = auxiliary ? 0 : 4;
      requestBlock(auxiliary ? 0x1E80 : 0, 64);
    }
    else if (received || expired(now)) fail("Radio refused read session");
    return;
  }
  if (phase == Phase::block) {
    size_t prefix = firstBlock ? 0 : 1;
    size_t expected = prefix + 4 + blockSize;
    if (received < expected) {
      if (expired(now)) fail("Incomplete memory block; check cable and power-cycle radio");
      return;
    }
    if (received != expected || (prefix && reply[0] != 0x06) || reply[prefix] != 0x58 ||
        reply[prefix + 1] != (address >> 8) || reply[prefix + 2] != (address & 255) ||
        reply[prefix + 3] != blockSize) { fail("Invalid memory block header or ACK"); return; }
    const uint8_t *data = reply + prefix + 4;
    if (probe < 3) {
      if (probe == 2) droppedByte = data[15] == 0xFF;
      ++probe;
    } else {
      size_t offset = address < 0x1800 ? address + 8 : 0x1808 + address - 0x1EC0;
      memcpy(image + offset, data, blockSize);
      completed += blockSize;
    }
    firstBlock = false;
    radioSerial.write(static_cast<uint8_t>(0x06));
    phase = Phase::pause;
    deadline = now + 50;
  }
}

const uint8_t *radioProgrammingImage(size_t &length) {
  length = phase == Phase::done ? (auxiliary ? sizeof(image) : 0x1808) : 0;
  return length ? image : nullptr;
}

String radioProgrammingJson(bool details) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddStringToObject(root, "state", radioProgrammingBusy() ? (writeStage == WriteStage::none ? "reading" : "writing") : phase == Phase::done ? "done" : phase == Phase::failed ? "error" : "idle");
  cJSON_AddStringToObject(root, "operation", writeStage == WriteStage::none ? "read" : "write");
  cJSON_AddStringToObject(root, "stage", writeStage == WriteStage::preflight ? "preflight" : writeStage == WriteStage::writing ? "write" : writeStage == WriteStage::verifying ? "verify" : "read");
  cJSON_AddStringToObject(root, "revision", radioProgrammingRevision().c_str());
  cJSON_AddBoolToObject(root, "writeSupported", radioProgrammingCanWrite());
  cJSON_AddBoolToObject(root, "writeVerified", writeVerified);
  addNumber(root, "bytesWritten", bytesWritten);
  addNumber(root, "writeBytes", writeBytes);
  cJSON_AddStringToObject(root, "error", failure);
  addNumber(root, "bytesRead", completed);
  addNumber(root, "totalBytes", auxiliary ? 0x1940 : 0x1800);
  addNumber(root, "rxGpio", 16);
  addNumber(root, "txGpio", 17);
  if (phase == Phase::done && details) {
    char firmware[15] = {};
    for (size_t index = 0; index < 14; ++index) {
      uint8_t value = image[0x1838 + index];
      firmware[index] = value >= 32 && value < 127 ? value : ' ';
    }
    cJSON_AddStringToObject(root, "firmware", auxiliary ? firmware : "Legacy (no auxiliary block)");
    cJSON_AddBoolToObject(root, "droppedByteWorkaround", droppedByte);
    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    addNumber(settings, "squelch", image[0xE28]);
    addNumber(settings, "save", image[0xE2B]);
    addNumber(settings, "vox", image[0xE2C]);
    addNumber(settings, "backlight", image[0xE2E]);
    addNumber(settings, "dualWatch", image[0xE2F]);
    addNumber(settings, "beep", image[0xE30]);
    addNumber(settings, "timeoutRaw", image[0xE31]);
    addNumber(settings, "stepRaw", image[0xE29]);
    addNumber(settings, "voiceRaw", image[0xE36]);
    addNumber(settings, "dtmfSidetoneRaw", image[0xE38]);
    addNumber(settings, "scanResumeRaw", image[0xE3A] & 3);
    addNumber(settings, "pttIdRaw", image[0xE3B]);
    addNumber(settings, "pttDelayRaw", image[0xE3C]);
    addNumber(settings, "displayARaw", image[0xE3D]);
    addNumber(settings, "displayBRaw", image[0xE3E]);
    addNumber(settings, "busyLockRaw", image[0xE3F]);
    addNumber(settings, "autoLockRaw", image[0xE40]);
    addNumber(settings, "shiftRaw", image[0xE41]);
    addNumber(settings, "standbyLedRaw", image[0xE45]);
    addNumber(settings, "rxLedRaw", image[0xE46]);
    addNumber(settings, "txLedRaw", image[0xE47]);
    addNumber(settings, "alarmModeRaw", image[0xE48]);
    addNumber(settings, "bandRaw", image[0xE49]);
    addNumber(settings, "txPriorityRaw", image[0xE4A]);
    addNumber(settings, "tailEliminateRaw", image[0xE4B]);
    addNumber(settings, "repeaterTailRaw", image[0xE4C]);
    addNumber(settings, "repeaterDelayRaw", image[0xE4D]);
    addNumber(settings, "powerOnModeRaw", image[0xE4E]);
    addNumber(settings, "rogerRaw", image[0xE4F]);
    addNumber(settings, "rogerRxRaw", image[0xE50]);
    addNumber(settings, "activeLineRaw", (image[0xE52] >> 7) & 1);
    addNumber(settings, "fmRadioEnabledRaw", (image[0xE52] >> 4) & 1);
    addNumber(settings, "alarmEnabledRaw", (image[0xE52] >> 3) & 1);
    addNumber(settings, "resetMenuEnabledRaw", (image[0xE52] >> 1) & 1);
    addNumber(settings, "menusEnabledRaw", image[0xE52] & 1);
    addNumber(settings, "workModeRaw", image[0xE54]);
    addNumber(settings, "keypadLockRaw", image[0xE55]);
    addNumber(settings, "channelA", image[0xE7E] & 127);
    addNumber(settings, "channelB", image[0xE7F] & 127);
    addVfo(settings, "vfoAHz", 0xF10);
    addVfo(settings, "vfoBHz", 0xF30);
    addVfoDetails(root, "vfoA", 0xF10);
    addVfoDetails(root, "vfoB", 0xF30);
    cJSON *dtmf = cJSON_AddObjectToObject(root, "dtmf");
    addDtmfCode(dtmf, "aniCode", 0xCB2, 5);
    addDtmfCode(dtmf, "alarmCode", 0xC92, 3);
    addNumber(dtmf, "aniModeRaw", image[0xCB7] & 3);
    addNumber(dtmf, "toneOnRaw", image[0xCBA]);
    addNumber(dtmf, "toneOffRaw", image[0xCBB]);
    for (uint8_t index = 0; index < 15; ++index) {
      char name[8];
      snprintf(name, sizeof(name), "code%u", index + 1);
      addDtmfCode(dtmf, name, 0xB08 + index * 16, 5);
    }
    if (auxiliary) {
      cJSON *startup = cJSON_AddObjectToObject(root, "startup");
      addText(startup, "line1", 0x1828, 7);
      addText(startup, "line2", 0x182F, 7);
      addText(startup, "alternateLine1", 0x1818, 7);
      addText(startup, "alternateLine2", 0x181F, 7);
    }
    cJSON *channels = cJSON_AddArrayToObject(root, "channels");
    for (uint16_t index = 0; index < 128; ++index) {
      const uint8_t *channel = image + 8 + index * 16;
      if (channel[0] == 0xFF) continue;
      cJSON *item = cJSON_CreateObject();
      addNumber(item, "number", index);
      char name[8] = {};
      for (uint8_t letter = 0; letter < 7; ++letter) {
        uint8_t value = image[0x1008 + index * 16 + letter];
        name[letter] = value >= 32 && value < 127 ? value : ' ';
      }
      cJSON_AddStringToObject(item, "name", name);
      addFrequency(item, "rxHz", channel);
      addFrequency(item, "txHz", channel + 4);
      addNumber(item, "rxToneRaw", channel[8] | channel[9] << 8);
      addNumber(item, "txToneRaw", channel[10] | channel[11] << 8);
      cJSON_AddStringToObject(item, "bandwidth", channel[15] & 0x40 ? "Wide" : "Narrow");
      addNumber(item, "powerRaw", channel[14] & 3);
      addNumber(item, "busyLockRaw", (channel[15] >> 3) & 1);
      addNumber(item, "pttIdRaw", channel[15] & 3);
      addNumber(item, "signalCodeRaw", channel[12] & 15);
      cJSON_AddBoolToObject(item, "scan", channel[15] & 4);
      cJSON_AddItemToArray(channels, item);
    }
  }
  char *encoded = cJSON_PrintUnformatted(root);
  String result = encoded ? encoded : "{}";
  cJSON_free(encoded);
  cJSON_Delete(root);
  return result;
}