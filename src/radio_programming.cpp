#include "radio_programming.h"
#include "ptt_test.h"
#include <HardwareSerial.h>
#include <cJSON.h>
#include <driver/gpio.h>

namespace {
HardwareSerial radioSerial(1);
enum class Phase { idle, magic, magicAck, ident, identAck, block, pause, retry, done, failed };
Phase phase = Phase::idle;
uint8_t image[0x1948] = {};
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
}

bool radioProgrammingBusy() {
  return phase != Phase::idle && phase != Phase::done && phase != Phase::failed;
}

bool radioProgrammingStart() {
  if (radioProgrammingBusy() || pttTestActive()) return false;
  memset(image, 0, sizeof(image));
  failure = "";
  completed = 0;
  magicIndex = 0;
  droppedByte = false;
  startedAt = millis();
  beginAttempt();
  return true;
}

void radioProgrammingPoll() {
  if (!radioProgrammingBusy()) return;
  uint32_t now = millis();
  if (now - startedAt > 45000) { fail("Read timed out; power-cycle the radio before retrying"); return; }
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
  if (phase == Phase::pause) {
    if (!expired(now)) return;
    if (probe == 1) { requestBlock(0x1EC0, 64); return; }
    if (probe == 2) { requestBlock(0x1FC0, 64); return; }
    if (probe == 3) { probe = 4; requestBlock(0, 64); return; }
    if (address < 0x1800 && address + blockSize < 0x1800) { requestBlock(address + blockSize, 64); return; }
    if (address < 0x1800 && auxiliary) { requestBlock(0x1EC0, 64); return; }
    if (address < 0x1800) { phase = Phase::done; releaseUart(); return; }
    if (address + blockSize < 0x2000) {
      uint16_t next = address + blockSize;
      requestBlock(next, droppedByte && next >= 0x1FC0 ? 16 : 64);
      return;
    }
    phase = Phase::done;
    releaseUart();
    return;
  }
  while (radioSerial.available() && received < sizeof(reply)) reply[received++] = radioSerial.read();
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

String radioProgrammingJson() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddStringToObject(root, "state", radioProgrammingBusy() ? "reading" : phase == Phase::done ? "done" : phase == Phase::failed ? "error" : "idle");
  cJSON_AddStringToObject(root, "error", failure);
  addNumber(root, "bytesRead", completed);
  addNumber(root, "totalBytes", auxiliary ? 0x1940 : 0x1800);
  addNumber(root, "rxGpio", 16);
  addNumber(root, "txGpio", 17);
  if (phase == Phase::done) {
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
    addNumber(settings, "channelA", image[0xE7E] & 127);
    addNumber(settings, "channelB", image[0xE7F] & 127);
    addVfo(settings, "vfoAHz", 0xF10);
    addVfo(settings, "vfoBHz", 0xF30);
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