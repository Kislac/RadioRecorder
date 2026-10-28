#pragma once

#include <stdint.h>

constexpr uint32_t kPttTestPulseMs = 1000;

void initPttTest();
bool startPttTest();
bool latchPttTest();
void stopPttTest();
void pollPttTest();
bool pttTestActive();
