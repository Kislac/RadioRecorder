#pragma once

#include <Arduino.h>

bool wifiManagerInit();
void wifiManagerPoll();
void wifiManagerNotifyActivity();
void wifiManagerWake();
bool wifiManagerToggle();
bool wifiManagerRequestScan();
String wifiManagerScanJson();
String wifiManagerStatusJson();
bool wifiManagerConnect(const String &ssid, const String &password, bool keepApHost);
bool wifiManagerForget();
String wifiManagerModeName();
String wifiManagerIPAddress();
String wifiManagerSSID();
String wifiManagerSavedSSID();
bool wifiManagerKeepApHost();
bool wifiManagerHasCredentials();
uint16_t wifiManagerAutoOffMinutes();
bool wifiManagerSetAutoOffMinutes(uint16_t minutes);