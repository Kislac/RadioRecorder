#include "language.h"
#include "app_control.h"
#include "language.generated.h"
#include <cstring>

bool appLanguageSupported(const char *code) {
  for (const auto &language : kLanguages) if (strcmp(language.code, code) == 0) return true;
  return false;
}

const char *appNextLanguage() {
  size_t count = sizeof(kLanguages) / sizeof(kLanguages[0]);
  for (size_t index = 0; index < count; ++index) {
    if (strcmp(kLanguages[index].code, appLanguage()) == 0) return kLanguages[(index + 1) % count].code;
  }
  return "hu";
}

const char *appLanguageName() {
  for (const auto &language : kLanguages) if (strcmp(language.code, appLanguage()) == 0) return language.name;
  return appLanguage();
}

const char *appText(const char *text) {
  for (const auto &language : kLanguages) {
    if (strcmp(language.code, appLanguage()) != 0) continue;
    for (size_t index = 0; index < language.count; ++index) {
      if (strcmp(language.entries[index].original, text) == 0) return language.entries[index].translated;
    }
  }
  return text;
}