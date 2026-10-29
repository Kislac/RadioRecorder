#include "radio_patch.h"
#include "radio_programming.h"
#include <cJSON.h>
#include <cmath>

namespace {
enum class Encoding { byte, word, frequency, digits, text, dtmf };
struct Field {
  const char *group;
  const char *name;
  uint16_t offset;
  uint32_t maximum;
  uint8_t mask;
  uint8_t shift;
  Encoding encoding;
};
const Field fields[] = {
  {"settings", "squelch", 0xE28, 9, 255, 0, Encoding::byte},
  {"settings", "save", 0xE2B, 4, 255, 0, Encoding::byte},
  {"settings", "vox", 0xE2C, 10, 255, 0, Encoding::byte},
  {"settings", "backlight", 0xE2E, 24, 255, 0, Encoding::byte},
  {"settings", "dualWatch", 0xE2F, 1, 255, 0, Encoding::byte},
  {"settings", "beep", 0xE30, 1, 255, 0, Encoding::byte},
  {"settings", "timeoutRaw", 0xE31, 39, 255, 0, Encoding::byte},
  {"settings", "voiceRaw", 0xE36, 2, 255, 0, Encoding::byte},
  {"settings", "dtmfSidetoneRaw", 0xE38, 3, 255, 0, Encoding::byte},
  {"settings", "scanResumeRaw", 0xE3A, 2, 3, 0, Encoding::byte},
  {"settings", "pttIdRaw", 0xE3B, 3, 255, 0, Encoding::byte},
  {"settings", "pttDelayRaw", 0xE3C, 50, 255, 0, Encoding::byte},
  {"settings", "displayARaw", 0xE3D, 2, 255, 0, Encoding::byte},
  {"settings", "displayBRaw", 0xE3E, 2, 255, 0, Encoding::byte},
  {"settings", "busyLockRaw", 0xE3F, 1, 255, 0, Encoding::byte},
  {"settings", "autoLockRaw", 0xE40, 1, 255, 0, Encoding::byte},
  {"settings", "standbyLedRaw", 0xE45, 3, 255, 0, Encoding::byte},
  {"settings", "rxLedRaw", 0xE46, 3, 255, 0, Encoding::byte},
  {"settings", "txLedRaw", 0xE47, 3, 255, 0, Encoding::byte},
  {"settings", "alarmModeRaw", 0xE48, 2, 255, 0, Encoding::byte},
  {"settings", "txPriorityRaw", 0xE4A, 2, 255, 0, Encoding::byte},
  {"settings", "tailEliminateRaw", 0xE4B, 1, 255, 0, Encoding::byte},
  {"settings", "repeaterTailRaw", 0xE4C, 10, 255, 0, Encoding::byte},
  {"settings", "repeaterDelayRaw", 0xE4D, 10, 255, 0, Encoding::byte},
  {"settings", "powerOnModeRaw", 0xE4E, 1, 255, 0, Encoding::byte},
  {"settings", "rogerRaw", 0xE4F, 1, 255, 0, Encoding::byte},
  {"settings", "activeLineRaw", 0xE52, 1, 128, 7, Encoding::byte},
  {"settings", "fmRadioEnabledRaw", 0xE52, 1, 16, 4, Encoding::byte},
  {"settings", "workModeRaw", 0xE54, 1, 255, 0, Encoding::byte},
  {"settings", "keypadLockRaw", 0xE55, 1, 255, 0, Encoding::byte},
  {"settings", "channelA", 0xE7E, 127, 127, 0, Encoding::byte},
  {"settings", "channelB", 0xE7F, 127, 127, 0, Encoding::byte},
  {"vfo", "frequencyHz", 0, 520000000, 0, 0, Encoding::digits},
  {"vfo", "offsetHz", 8, 999999000, 0, 0, Encoding::digits},
  {"vfo", "rxToneRaw", 14, 65535, 0, 0, Encoding::word},
  {"vfo", "txToneRaw", 16, 65535, 0, 0, Encoding::word},
  {"vfo", "shiftRaw", 20, 2, 48, 4, Encoding::byte},
  {"vfo", "signalCodeRaw", 20, 14, 15, 0, Encoding::byte},
  {"vfo", "stepRaw", 22, 7, 112, 4, Encoding::byte},
  {"vfo", "powerRaw", 23, 1, 128, 7, Encoding::byte},
  {"vfo", "bandwidthRaw", 23, 1, 64, 6, Encoding::byte},
  {"channel", "rxHz", 0, 520000000, 0, 0, Encoding::frequency},
  {"channel", "txHz", 4, 520000000, 0, 0, Encoding::frequency},
  {"channel", "rxToneRaw", 8, 65535, 0, 0, Encoding::word},
  {"channel", "txToneRaw", 10, 65535, 0, 0, Encoding::word},
  {"channel", "signalCodeRaw", 12, 14, 15, 0, Encoding::byte},
  {"channel", "powerRaw", 14, 1, 3, 0, Encoding::byte},
  {"channel", "bandwidthRaw", 15, 1, 64, 6, Encoding::byte},
  {"channel", "busyLockRaw", 15, 1, 8, 3, Encoding::byte},
  {"channel", "scanRaw", 15, 1, 4, 2, Encoding::byte},
  {"channel", "pttIdRaw", 15, 3, 3, 0, Encoding::byte},
  {"channel", "name", 0, 7, 0, 0, Encoding::text},
  {"dtmf", "aniCode", 0xCB2, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "alarmCode", 0xC92, 3, 0, 0, Encoding::dtmf},
  {"dtmf", "aniModeRaw", 0xCB7, 3, 3, 0, Encoding::byte},
  {"dtmf", "toneOnRaw", 0xCBA, 195, 255, 0, Encoding::byte},
  {"dtmf", "toneOffRaw", 0xCBB, 195, 255, 0, Encoding::byte},
  {"dtmf", "code1", 0xB08, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code2", 0xB18, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code3", 0xB28, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code4", 0xB38, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code5", 0xB48, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code6", 0xB58, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code7", 0xB68, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code8", 0xB78, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code9", 0xB88, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code10", 0xB98, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code11", 0xBA8, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code12", 0xBB8, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code13", 0xBC8, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code14", 0xBD8, 5, 0, 0, Encoding::dtmf},
  {"dtmf", "code15", 0xBE8, 5, 0, 0, Encoding::dtmf}
};

bool integer(cJSON *item, uint32_t maximum, uint32_t &value) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
      item->valuedouble > maximum || std::floor(item->valuedouble) != item->valuedouble) return false;
  value = static_cast<uint32_t>(item->valuedouble);
  return true;
}

bool validFrequency(uint32_t hz) {
  return hz % 10 == 0 && ((hz >= 130000000 && hz <= 176000000) || (hz >= 400000000 && hz <= 520000000));
}

bool validTone(uint32_t value) {
  if (value <= 210 || value == 65535) return true;
  const uint16_t tones[] = {670,693,719,744,770,797,825,854,885,915,948,974,1000,1035,1072,1109,
    1148,1188,1230,1273,1318,1365,1413,1462,1514,1567,1598,1622,1655,1679,1713,1738,1773,1799,
    1835,1862,1899,1928,1966,1995,2035,2065,2107,2181,2257,2291,2336,2418,2503,2541};
  for (uint16_t tone : tones) if (value == tone) return true;
  return false;
}

bool knownKeys(cJSON *object, const char *const *keys, size_t count) {
  if (!cJSON_IsObject(object)) return false;
  for (cJSON *item = object->child; item; item = item->next) {
    if (!item->string) return false;
    bool found = false;
    for (size_t index = 0; index < count; ++index) if (!strcmp(item->string, keys[index])) found = true;
    if (!found) return false;
    for (cJSON *prior = object->child; prior != item; prior = prior->next)
      if (!strcmp(prior->string, item->string)) return false;
  }
  return true;
}

bool encodeChange(uint8_t *output, cJSON *change) {
  const char *const keys[] = {"group", "field", "index", "value"};
  if (!knownKeys(change, keys, 4)) return false;
  cJSON *groupItem = cJSON_GetObjectItemCaseSensitive(change, "group");
  cJSON *fieldItem = cJSON_GetObjectItemCaseSensitive(change, "field");
  cJSON *item = cJSON_GetObjectItemCaseSensitive(change, "value");
  if (!cJSON_IsString(groupItem) || !cJSON_IsString(fieldItem) || !item) return false;
  const char *group = groupItem->valuestring;
  uint16_t base = 0;
  uint32_t channel = 0;
  cJSON *indexItem = cJSON_GetObjectItemCaseSensitive(change, "index");
  if (!strcmp(group, "channel")) {
    if (!integer(indexItem, 127, channel) || output[8 + channel * 16] == 255) return false;
    base = 8 + channel * 16;
  } else {
    if (indexItem) return false;
    if (!strcmp(group, "vfoA") || !strcmp(group, "vfoB")) {
      base = !strcmp(group, "vfoA") ? 0xF10 : 0xF30;
      group = "vfo";
    }
  }
  const Field *field = nullptr;
  for (const Field &candidate : fields)
    if (!strcmp(candidate.group, group) && !strcmp(candidate.name, fieldItem->valuestring)) field = &candidate;
  if (!field) return false;
  uint16_t offset = base + field->offset;
  if (field->encoding == Encoding::text || field->encoding == Encoding::dtmf) {
    if (!cJSON_IsString(item) || strlen(item->valuestring) > field->maximum) return false;
    const char *alphabet = field->encoding == Encoding::text ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !@#$%^&*()+-=[]:\";'<>?,./" :
                           !strcmp(field->name, "aniCode") ? "0123456789" : "0123456789 *#ABCD";
    if (field->encoding == Encoding::text) offset = 0x1008 + channel * 16;
    for (const char *letter = item->valuestring; *letter; ++letter) if (!strchr(alphabet, *letter)) return false;
    memset(output + offset, 255, field->maximum);
    for (size_t index = 0; index < strlen(item->valuestring); ++index)
      output[offset + index] = field->encoding == Encoding::text ? item->valuestring[index] : strchr(alphabet, item->valuestring[index]) - alphabet;
    return true;
  }
  if (field->encoding == Encoding::frequency && !strcmp(field->name, "txHz") && cJSON_IsNull(item)) {
    memset(output + offset, 255, 4);
    return true;
  }
  uint32_t value = 0;
  if (!integer(item, field->maximum, value)) return false;
  if (field->encoding == Encoding::byte) {
    output[offset] = (output[offset] & ~field->mask) | ((value << field->shift) & field->mask);
  } else if (field->encoding == Encoding::word) {
    if (!validTone(value)) return false;
    output[offset] = value & 255;
    output[offset + 1] = value >> 8;
  } else if (field->encoding == Encoding::frequency) {
    if (!validFrequency(value)) return false;
    if (!strcmp(field->name, "rxHz")) output[base + 12] = (output[base + 12] & ~16) | (value >= 400000000 ? 16 : 0);
    value /= 10;
    for (uint8_t index = 0; index < 4; ++index) {
      output[offset + index] = ((value / 10 % 10) << 4) | (value % 10);
      value /= 100;
    }
  } else {
    bool frequency = !strcmp(field->name, "frequencyHz");
    if (frequency && !validFrequency(value)) return false;
    if (!frequency && value % 1000) return false;
    if (frequency) output[base + 18] = (output[base + 18] & ~1) | (value >= 400000000 ? 1 : 0);
    value /= frequency ? 10 : 1000;
    for (int index = frequency ? 7 : 5; index >= 0; --index) { output[offset + index] = value % 10; value /= 10; }
  }
  return true;
}
}

String radioPatchSchemaJson() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON *list = cJSON_AddArrayToObject(root, "fields");
  for (const Field &field : fields) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", field.group);
    cJSON_AddStringToObject(item, "field", field.name);
    cJSON_AddNumberToObject(item, "max", field.maximum);
    cJSON_AddStringToObject(item, "kind", field.encoding == Encoding::text || field.encoding == Encoding::dtmf ? "text" :
      field.encoding == Encoding::word ? "tone" : field.encoding == Encoding::frequency || field.encoding == Encoding::digits ? "frequency" : "number");
    cJSON_AddItemToArray(list, item);
  }
  char *json = cJSON_PrintUnformatted(root);
  String result = json ? json : "{}";
  cJSON_free(json);
  cJSON_Delete(root);
  return result;
}

bool radioProgrammingWriteJson(const String &payload, String &error) {
  if (payload.length() > 4096) { error = "Patch exceeds 4096 bytes"; return false; }
  cJSON *root = cJSON_ParseWithOpts(payload.c_str(), nullptr, true);
  const char *const keys[] = {"revision", "changes"};
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  cJSON *changes = cJSON_GetObjectItemCaseSensitive(root, "changes");
  size_t length = 0;
  const uint8_t *source = radioProgrammingImage(length);
  static uint8_t prepared[0x1948];
  bool valid = source && length == sizeof(prepared) && knownKeys(root, keys, 2) && cJSON_IsString(revision) &&
               cJSON_IsArray(changes) && cJSON_GetArraySize(changes) > 0 && cJSON_GetArraySize(changes) <= 32;
  if (valid) {
    memcpy(prepared, source, length);
    for (cJSON *change = changes->child; change && valid; change = change->next) valid = encodeChange(prepared, change);
  }
  if (!valid) { error = "Invalid patch, unknown field, value or missing complete backup"; cJSON_Delete(root); return false; }
  bool started = radioProgrammingWriteImage(prepared, length, revision->valuestring, error);
  cJSON_Delete(root);
  return started;
}