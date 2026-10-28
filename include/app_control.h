#pragma once

#include <Arduino.h>

void appSetSdReady(bool ready);
bool appSdReady();
bool appStartRecording();
bool appStopRecording();
bool appCanStartPlayback();
bool appCanConnectLiveAudio();
bool appStartWifiTest();
bool appWifiTestActive();
void appPollWifiTest();
String appFirmwareVersion();
String appFirmwareBuildDateTime();
uint32_t appDemoTimeSeconds();
bool appInitSettings();
uint16_t appScreenOffMinutes();
bool appSetScreenOffMinutes(uint16_t minutes);