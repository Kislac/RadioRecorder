#pragma once

#include <Arduino.h>

void initAutomaticReplay(bool sdReady);
void automaticReplayStop();
bool automaticReplayArmed();
bool automaticReplayRequestArm(String &error);
bool automaticReplayArmPending();
bool automaticReplayConfirmArm();
bool automaticReplayReadyToKey();
void pollAutomaticReplay();
bool automaticReplaySaveRule(const String &json, String &error);
bool automaticReplaySetRuleActive(uint32_t id, bool enabled, String &error);
String automaticReplayRulesJson();