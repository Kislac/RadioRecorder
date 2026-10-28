#pragma once

bool initSystemWatchdog();
void feedSystemWatchdog();
bool watchdogSubscribeCurrentTask();
void watchdogUnsubscribeCurrentTask(bool subscribedHere);
void watchdogResetCurrentTask();