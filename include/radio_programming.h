#pragma once

#include <Arduino.h>

bool radioProgrammingBusy();
bool radioProgrammingStart();
void radioProgrammingPoll();
String radioProgrammingJson(bool details = true);
const uint8_t *radioProgrammingImage(size_t &length);
String radioProgrammingRevision();
bool radioProgrammingCanWrite();
bool radioProgrammingWriteImage(const uint8_t *desired, size_t length, const char *revision, String &error);