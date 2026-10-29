#pragma once

#include <Arduino.h>

String radioPatchSchemaJson();
bool radioProgrammingWriteJson(const String &payload, String &error);