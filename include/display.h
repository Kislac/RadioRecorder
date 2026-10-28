#pragma once

#include <stdint.h>
#include "controls.h"

void initDisplay();
bool displayReady();
void displaySleep();
void displayWake();
void pollDisplay(int32_t steps, ButtonEvent button);
void printDisplayFrame();
void printDisplayState();