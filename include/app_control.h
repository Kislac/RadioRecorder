#pragma once

#include <Arduino.h>
#include <time.h>

void appSetSdReady(bool ready);
bool appSdReady();
bool appStartRecording();
bool appStartMicRecording();
bool appStopRecording();
bool appCanStartPlayback();
bool appCanConnectLiveAudio();
bool appStartWifiTest();
bool appWifiTestActive();
void appPollWifiTest();
String appFirmwareVersion();
String appFirmwareBuildDateTime();
uint32_t appDemoTimeSeconds();
bool appClockKnown();
bool appClockLocal(tm &local);
char appClockIndicator();
String appClockJson();
uint8_t appClockTimezone();
bool appClockSetTimezone(uint8_t zone);
bool appClockSetLocal(const String &local);
void appClockStartSync();
void appClockStopSync();
void appClockPoll();
bool appInitSettings();
uint16_t appScreenOffMinutes();
bool appSetScreenOffMinutes(uint16_t minutes);