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
bool performanceLogSdIdle();
bool performanceLogValidPath(const String &path);
void performancePrimeLogCount();
void performanceLogPoll();
String performanceJson();