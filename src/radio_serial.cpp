#include "radio_serial.h"
#include "radio_programming.h"
#include "radio_patch.h"
#include "app_control.h"
#include <Arduino.h>
#include <cJSON.h>
#include <cmath>

namespace {
char frame[4608] = {};
size_t used = 0;
bool receiving = false;
bool discarded = false;
uint32_t lastByte = 0;

void respond(uint32_t id, const String &json) {
  char prefix[32];
  snprintf(prefix, sizeof(prefix), "\nRR_REPLY %lu ", static_cast<unsigned long>(id));
  String response(prefix);
  response += json;
  response += '\n';
  Serial.print(response);
}

void reject(uint32_t id, const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "error", message);
  char *json = cJSON_PrintUnformatted(root);
  respond(id, json ? json : "{\"error\":\"Out of memory\"}");
  cJSON_free(json);
  cJSON_Delete(root);
}

bool requestInteger(cJSON *item, uint32_t maximum, uint32_t &value) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
      item->valuedouble > maximum || std::floor(item->valuedouble) != item->valuedouble) return false;
  value = static_cast<uint32_t>(item->valuedouble);
  return true;
}

void execute() {
  if (discarded || used < 3 || memcmp(frame, "RR ", 3)) { reject(0, "Invalid, oversized or timed-out frame"); return; }
  cJSON *request = cJSON_ParseWithOpts(frame + 3, nullptr, true);
  uint32_t id = 0;
  cJSON *command = cJSON_GetObjectItemCaseSensitive(request, "command");
  if (!cJSON_IsObject(request) || !requestInteger(cJSON_GetObjectItemCaseSensitive(request, "id"), 999999, id) || !cJSON_IsString(command)) {
    cJSON_Delete(request);
    reject(0, "Invalid request");
    return;
  }
  const char *name = command->valuestring;
  if (!strcmp(name, "hello")) respond(id, "{\"protocol\":1}");
  else if (!strcmp(name, "status")) respond(id, radioProgrammingJson(false));
  else if (!strcmp(name, "settings")) respond(id, radioProgrammingJson());
  else if (!strcmp(name, "schema")) respond(id, radioPatchSchemaJson());
  else if (!strcmp(name, "read")) {
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "confirm"))) reject(id, "Wiring confirmation required");
    else if (appWifiTestActive() || !radioProgrammingStart()) reject(id, "Radio, PTT or Wi-Fi test busy");
    else respond(id, radioProgrammingJson(false));
  } else if (!strcmp(name, "write")) {
    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(request, "confirm");
    cJSON *patch = cJSON_GetObjectItemCaseSensitive(request, "patch");
    String error;
    char *payload = cJSON_IsObject(patch) ? cJSON_PrintUnformatted(patch) : nullptr;
    if (!cJSON_IsString(confirm) || strcmp(confirm->valuestring, "WRITE UV5R")) reject(id, "WRITE UV5R confirmation required");
    else if (appWifiTestActive()) reject(id, "Wi-Fi test active");
    else if (!payload || !radioProgrammingWriteJson(payload, error)) reject(id, error.length() ? error.c_str() : "Invalid patch");
    else respond(id, radioProgrammingJson(false));
    cJSON_free(payload);
  } else if (!strcmp(name, "image")) {
    size_t length = 0;
    const uint8_t *image = radioProgrammingImage(length);
    uint32_t offset = 0;
    uint32_t count = 0;
    if (!image || !requestInteger(cJSON_GetObjectItemCaseSensitive(request, "offset"), length, offset) ||
      !requestInteger(cJSON_GetObjectItemCaseSensitive(request, "count"), 128, count) || !count || offset + count > length) {
      reject(id, "Invalid image range or no complete image");
    } else {
      const char alphabet[] = "0123456789ABCDEF";
      char hex[257] = {};
      for (uint32_t index = 0; index < count; ++index) {
        hex[index * 2] = alphabet[image[offset + index] >> 4];
        hex[index * 2 + 1] = alphabet[image[offset + index] & 15];
      }
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "offset", offset);
      cJSON_AddNumberToObject(root, "total", length);
      cJSON_AddStringToObject(root, "revision", radioProgrammingRevision().c_str());
      cJSON_AddStringToObject(root, "data", hex);
      char *json = cJSON_PrintUnformatted(root);
      respond(id, json ? json : "{}");
      cJSON_free(json);
      cJSON_Delete(root);
    }
  } else reject(id, "Unknown command");
  cJSON_Delete(request);
}
}

bool radioSerialReceiving() { return receiving; }

void radioSerialPoll() {
  if (receiving && millis() - lastByte > 5000) discarded = true;
}

bool radioSerialConsume(char value) {
  if (!receiving) {
    if (value != '@') return false;
    receiving = true;
    discarded = false;
    used = 0;
  } else if (value == '\n') {
    frame[used] = '\0';
    receiving = false;
    execute();
  } else if (value != '\r') {
    if (used < sizeof(frame) - 1) frame[used++] = value;
    else discarded = true;
  }
  lastByte = millis();
  return true;
}