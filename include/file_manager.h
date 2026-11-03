#pragma once

#include <Arduino.h>
#include "file_rules.h"

struct ManagedFilePage {
  std::vector<ManagedFileInfo> items;
  uint32_t total = 0;
  String revision;
  String next;
  String error;
};

bool fileManagerList(const String &directory, const String &sort, bool descending, const String &filter,
                     const String &after, uint8_t limit, const String &revision, ManagedFilePage &page, bool wavOnly = false);
bool fileManagerListOffset(const String &directory, uint32_t offset, uint8_t limit, ManagedFilePage &page);
bool fileManagerBusy();
uint32_t fileManagerLastStackFreeMinimum();
bool fileManagerMkdir(const String &path, String &error);
bool fileManagerRename(const String &source, const String &destination, String &error);
bool fileManagerDeleteFile(const String &path, String &error);
uint32_t fileManagerStartJob(const String &action, const String &source, const String &destination, uint64_t bytes, String &error);
String fileManagerJobJson(uint32_t id);
bool fileManagerCancelJob(uint32_t id);
void fileManagerStartServices();
void fileManagerStopServices();
void pollFileManager();