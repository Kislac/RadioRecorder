#pragma once

#include <stdint.h>

enum class ButtonEvent { none, shortPress, longPress };

void initControls();
int32_t takeEncoderSteps();
ButtonEvent pollEncoderButton();