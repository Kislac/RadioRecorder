#pragma once

#include <stdint.h>
#include "controls.h"

void initDisplay();
bool displayReady();
void pollDisplay(int32_t steps, ButtonEvent button);