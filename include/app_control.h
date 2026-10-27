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