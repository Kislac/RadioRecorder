#pragma once

#include <Arduino.h>

bool hasRecordings();
bool safeSdPath(const String &path);
void initWeb();
void webStartServices();
void webStopServices();
void pollWeb();
bool webReady();
bool webDisableForNoiseTest();