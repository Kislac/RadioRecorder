#pragma once

#include <Arduino.h>

bool radioProgrammingBusy();
bool radioProgrammingStart();
void radioProgrammingPoll();
String radioProgrammingJson();
const uint8_t *radioProgrammingImage(size_t &length);