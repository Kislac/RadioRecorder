#pragma once

#include <Arduino.h>

bool performanceInit();
bool performanceEnabled();
bool performanceSetEnabled(bool enabled);
void performanceLoopTick();
String performanceJson();