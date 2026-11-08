#pragma once

#include <Arduino.h>

constexpr char kPerformanceLogDirectory[] = "/performance-logs";

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