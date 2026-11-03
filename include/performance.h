#pragma once

#include <Arduino.h>

bool performanceInit();
bool performanceEnabled();
bool performanceSetEnabled(bool enabled);
void performanceLoopStart();
void performanceLoopTick();
void performanceDisplaySample(uint32_t durationUs);
bool performanceLogSetEnabled(bool enabled);
bool performanceLogEnabled();
const char *performanceLogPath();
void performanceLogPoll();
String performanceJson();