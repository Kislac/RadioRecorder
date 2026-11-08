#include "automatic_replay.h"

#include <SD_MMC.h>
#include <atomic>
#include <cJSON.h>
#include <cstring>
#include "app_control.h"
#include "automatic_record.h"
#include "display.h"
#include "file_manager.h"
#include "file_rules.h"
#include "playback.h"
#include "ptt_test.h"
#include "radio_programming.h"
#include "recorder.h"
#include "replay_rules.h"
#include "wav.h"

namespace {
constexpr size_t kMaxRules = 16;
constexpr size_t kMaxConfigBytes = 8192;
constexpr char kConfigPath[] = "/replay-rules.json";
constexpr char kBackupPath[] = "/replay-rules.bak";
constexpr char kPendingPath[] = "/replay-rules.tmp";

struct ReplayRule {
  uint32_t id = 0;
  bool active = false;
  bool completed = false;
  char path[221] = {};
  bool folder = false;
  bool shuffled = false;
  bool loop = false;
  bool all = false;
  char cursor[221] = {};
  uint32_t round = 0;
  uint16_t fileCount = 0;
  uint32_t fingerprint = 0;
  ReplaySchedule schedule;
};

ReplayRule rules[kMaxRules];
ReplayRule verifiedRules[kMaxRules];
size_t ruleCount = 0;
bool storageValid = false;
uint32_t nextId = 1;
constexpr size_t kQueueLimit = 32;
constexpr uint32_t kSilenceMs = 10000;
struct ReplayEvent { uint8_t ruleIndex; uint32_t occurrence; int64_t dueUtc; uint16_t remaining; };
ReplayEvent queue[kQueueLimit];
size_t queueCount = 0;
uint32_t nextOccurrence[kMaxRules] = {};
std::atomic<int> playingRule{-1};
char playingFile[221] = {};
std::atomic<uint32_t> quietSince{0}, lastSampleAt{0};
bool signalOwned = false;
uint32_t lastSequence = 0;
uint32_t requestedAt = 0;
bool armPending = false;
String replayError;

bool clockTrusted() {
  if (!appClockKnown()) return false;
  char source = appClockIndicator();
  return source == 'R' || source == 'N' || source == 'K' || source == 'T';
}

bool inWindow(const ReplayRule &rule, int64_t instant) {
  time_t epoch = static_cast<time_t>(instant);
  tm local = {};
  return localtime_r(&epoch, &local) &&
         replayWindowContains(static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min), rule.schedule);
}

void stopSignal() {
  if (signalOwned) recorderSetSignal(false);
  signalOwned = false;
  quietSince.store(0);
  lastSampleAt.store(0);
  lastSequence = 0;
}

void enqueue(const ReplayEvent &event) {
  size_t position = queueCount++;
  while (position && (queue[position - 1].dueUtc > event.dueUtc ||
         (queue[position - 1].dueUtc == event.dueUtc &&
          rules[queue[position - 1].ruleIndex].id > rules[event.ruleIndex].id))) {
    queue[position] = queue[position - 1];
    --position;
  }
  queue[position] = event;
}

void popEvent(bool resetSilence = true) {
  for (size_t index = 1; index < queueCount; ++index) queue[index - 1] = queue[index];
  if (queueCount) --queueCount;
  if (resetSilence) quietSince.store(0);
}

String replayBaseName(const File &entry) {
  String name = entry.name();
  return name.substring(name.lastIndexOf('/') + 1);
}

bool folderSnapshot(const ReplayRule &rule, uint16_t &count, uint32_t &fingerprint, bool checkAudio) {
  File directory = SD_MMC.open(rule.path, FILE_READ);
  if (!directory || !directory.isDirectory()) { directory.close(); return false; }
  count = 0;
  fingerprint = 0;
  uint16_t scanned = 0;
  File entry;
  bool valid = true;
  while ((entry = directory.openNextFile())) {
    if (++scanned > 512) { valid = false; entry.close(); break; }
    String name = replayBaseName(entry), extension = name;
    extension.toLowerCase();
    if (!entry.isDirectory() && extension.endsWith(".wav")) {
      String path = String(rule.path) == "/" ? "/" + name : String(rule.path) + "/" + name;
      uint32_t remaining = 0;
      if (++count > 128 || !managedPathValid(path.c_str(), false) ||
          (checkAudio && !findPcmData(entry, remaining))) valid = false;
      fingerprint += replayFileHash(name.c_str(), rule.id) ^ static_cast<uint32_t>(entry.size());
    }
    entry.close();
    if (!valid) break;
  }
  directory.close();
  return valid && count > 0;
}

bool folderSelect(const ReplayRule &rule, String &selected) {
  File directory = SD_MMC.open(rule.path, FILE_READ);
  if (!directory || !directory.isDirectory()) { directory.close(); return false; }
  String next, first;
  uint32_t seed = rule.id ^ rule.round;
  File entry;
  while ((entry = directory.openNextFile())) {
    String name = replayBaseName(entry), extension = name;
    extension.toLowerCase();
    if (!entry.isDirectory() && extension.endsWith(".wav")) {
        if (first.isEmpty() || replayFileBefore(name.c_str(), first.c_str(), rule.shuffled, seed)) first = name;
          if (replayFileAfterCursor(name.c_str(), rule.cursor, next.c_str(), rule.shuffled, seed)) next = name;
    }
    entry.close();
  }
  directory.close();
  if (next.isEmpty() && rule.loop) next = first;
  if (next.isEmpty()) return false;
  selected = String(rule.path) == "/" ? "/" + next : String(rule.path) + "/" + next;
  return selected.length() <= 220;
}

bool integerField(const cJSON *object, const char *name, int64_t minimum, int64_t maximum, int64_t &out) {
  const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(field) || field->valuedouble < minimum || field->valuedouble > maximum ||
      static_cast<double>(static_cast<int64_t>(field->valuedouble)) != field->valuedouble) return false;
  out = static_cast<int64_t>(field->valuedouble);
  return true;
}

bool decodeRule(const cJSON *object, ReplayRule &rule) {
  int64_t id, anchor, interval, jitter, window, start, end;
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(object, "path");
  const cJSON *active = cJSON_GetObjectItemCaseSensitive(object, "active");
  const cJSON *completed = cJSON_GetObjectItemCaseSensitive(object, "completed");
  String extension = cJSON_IsString(path) ? String(path->valuestring) : String();
  extension.toLowerCase();
  const cJSON *folder = cJSON_GetObjectItemCaseSensitive(object, "folder");
  if (!integerField(object, "id", 1, UINT32_MAX - 1, id) || !cJSON_IsString(path) ||
      !managedPathValid(path->valuestring) || (!cJSON_IsTrue(folder) && !extension.endsWith(".wav")) ||
      (folder && !cJSON_IsBool(folder)) ||
      !cJSON_IsBool(active) || !cJSON_IsBool(completed) ||
      !integerField(object, "anchorUtc", 1577836800, 4102444800LL, anchor) ||
      !integerField(object, "intervalSeconds", 0, 604800, interval) ||
      !integerField(object, "jitterMinutes", 0, 10079, jitter) ||
      !integerField(object, "window", 0, 2, window) ||
      !integerField(object, "startMinute", 0, 1439, start) ||
      !integerField(object, "endMinute", 0, 1439, end)) return false;
  rule.id = static_cast<uint32_t>(id);
  rule.active = cJSON_IsTrue(active);
  rule.completed = cJSON_IsTrue(completed);
  rule.folder = cJSON_IsTrue(folder);
  if (rule.folder) {
    int64_t order, cycle, batch, count, signature, round;
    const cJSON *cursor = cJSON_GetObjectItemCaseSensitive(object, "cursor");
    String cursorPath = String(path->valuestring) == "/" ? "/" : String(path->valuestring) + "/";
    if (cJSON_IsString(cursor)) cursorPath += cursor->valuestring;
    if (!integerField(object, "order", 0, 1, order) || !integerField(object, "cycle", 0, 1, cycle) ||
        !integerField(object, "batch", 0, 1, batch) || !integerField(object, "fileCount", 0, 128, count) ||
        !integerField(object, "fingerprint", 0, UINT32_MAX, signature) ||
        !integerField(object, "round", 0, UINT32_MAX, round) || !cJSON_IsString(cursor) ||
        (cursor->valuestring[0] && (strchr(cursor->valuestring, '/') ||
          !managedPathValid(cursorPath.c_str(), false)))) return false;
    rule.shuffled = order;
    rule.loop = cycle;
    rule.all = batch;
    rule.fileCount = count;
    rule.fingerprint = signature;
    rule.round = round;
    String(cursor->valuestring).toCharArray(rule.cursor, sizeof(rule.cursor));
    if (rule.active && !rule.fileCount) return false;
  }
  String(path->valuestring).toCharArray(rule.path, sizeof(rule.path));
  rule.schedule.anchorUtc = anchor;
  rule.schedule.intervalSeconds = interval;
  rule.schedule.jitterMinutes = jitter;
  rule.schedule.window = static_cast<ReplayWindow>(window);
  rule.schedule.startMinute = start;
  rule.schedule.endMinute = end;
  return replayScheduleValid(rule.schedule) && !(rule.completed && rule.active);
}

cJSON *encodeRule(const ReplayRule &rule) {
  cJSON *object = cJSON_CreateObject();
  if (!object) return nullptr;
  cJSON_AddNumberToObject(object, "id", rule.id);
  cJSON_AddStringToObject(object, "path", rule.path);
  cJSON_AddBoolToObject(object, "active", rule.active);
  cJSON_AddBoolToObject(object, "completed", rule.completed);
  if (rule.folder) {
    cJSON_AddBoolToObject(object, "folder", true);
    cJSON_AddNumberToObject(object, "order", rule.shuffled ? 1 : 0);
    cJSON_AddNumberToObject(object, "cycle", rule.loop ? 1 : 0);
    cJSON_AddNumberToObject(object, "batch", rule.all ? 1 : 0);
    cJSON_AddStringToObject(object, "cursor", rule.cursor);
    cJSON_AddNumberToObject(object, "fileCount", rule.fileCount);
    cJSON_AddNumberToObject(object, "fingerprint", rule.fingerprint);
    cJSON_AddNumberToObject(object, "round", rule.round);
  }
  cJSON_AddNumberToObject(object, "anchorUtc", static_cast<double>(rule.schedule.anchorUtc));
  cJSON_AddNumberToObject(object, "intervalSeconds", rule.schedule.intervalSeconds);
  cJSON_AddNumberToObject(object, "jitterMinutes", rule.schedule.jitterMinutes);
  cJSON_AddNumberToObject(object, "window", static_cast<int>(rule.schedule.window));
  cJSON_AddNumberToObject(object, "startMinute", rule.schedule.startMinute);
  cJSON_AddNumberToObject(object, "endMinute", rule.schedule.endMinute);
  return object;
}

bool decodeConfig(const String &text, ReplayRule *target, size_t &count, uint32_t &followingId) {
  cJSON *root = cJSON_Parse(text.c_str());
  if (!root) return false;
  int64_t version = 0, next = 1;
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "rules");
  bool valid = integerField(root, "version", 1, 1, version) &&
               integerField(root, "nextId", 1, UINT32_MAX, next) &&
               cJSON_IsArray(items) && cJSON_GetArraySize(items) <= static_cast<int>(kMaxRules);
  count = 0;
  if (valid) {
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
      if (!decodeRule(item, target[count]) || target[count].id >= static_cast<uint32_t>(next)) { valid = false; break; }
      for (size_t previous = 0; previous < count; ++previous)
        if (target[previous].id == target[count].id) valid = false;
      if (!valid) break;
      ++count;
    }
  }
  followingId = static_cast<uint32_t>(next);
  cJSON_Delete(root);
  return valid;
}

bool readConfig(const char *path, ReplayRule *target, size_t &count, uint32_t &followingId) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > kMaxConfigBytes) { file.close(); return false; }
  String text;
  text.reserve(file.size() + 1);
  while (file.available()) text += static_cast<char>(file.read());
  file.close();
  return decodeConfig(text, target, count, followingId);
}

bool writeConfig() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return false;
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddNumberToObject(root, "nextId", nextId);
  cJSON *items = cJSON_AddArrayToObject(root, "rules");
  bool valid = items != nullptr;
  for (size_t index = 0; index < ruleCount && valid; ++index) {
    cJSON *item = encodeRule(rules[index]);
    if (!item) valid = false;
    else cJSON_AddItemToArray(items, item);
  }
  char *serialized = valid ? cJSON_PrintUnformatted(root) : nullptr;
  cJSON_Delete(root);
  if (!serialized) return false;
  size_t length = strlen(serialized);
  if (length > kMaxConfigBytes) { cJSON_free(serialized); return false; }
  SD_MMC.remove(kPendingPath);
  File file = SD_MMC.open(kPendingPath, FILE_WRITE);
  bool saved = file && file.write(reinterpret_cast<const uint8_t *>(serialized), length) == length;
  if (file) { file.flush(); file.close(); }
  if (saved) {
    File check = SD_MMC.open(kPendingPath, FILE_READ);
    saved = check && check.size() == length;
    for (size_t index = 0; index < length && saved; ++index)
      saved = check.read() == static_cast<uint8_t>(serialized[index]);
    if (check) check.close();
  }
  cJSON_free(serialized);
  size_t verifiedCount = 0;
  uint32_t verifiedNext = 0;
  saved = saved && readConfig(kPendingPath, verifiedRules, verifiedCount, verifiedNext) &&
          verifiedCount == ruleCount && verifiedNext == nextId;
  if (!saved) { SD_MMC.remove(kPendingPath); return false; }
  bool backedUp = SD_MMC.exists(kConfigPath);
  size_t priorCount = 0;
  uint32_t priorNext = 1;
  if (backedUp && !readConfig(kConfigPath, verifiedRules, priorCount, priorNext)) {
    if (!SD_MMC.remove(kConfigPath)) return false;
    backedUp = false;
  }
  if (backedUp) {
    if (SD_MMC.exists(kBackupPath) && !SD_MMC.remove(kBackupPath)) return false;
    if (!SD_MMC.rename(kConfigPath, kBackupPath)) return false;
  }
  if (!SD_MMC.rename(kPendingPath, kConfigPath)) {
    if (backedUp) SD_MMC.rename(kBackupPath, kConfigPath);
    return false;
  }
  return true;
}
}

static std::atomic<bool> replayArmed{false};

void initAutomaticReplay(bool sdReady) {
  replayArmed.store(false);
  armPending = false;
  queueCount = 0;
  playingRule.store(-1);
  ruleCount = 0;
  nextId = 1;
  storageValid = sdReady && !SD_MMC.exists(kConfigPath) && !SD_MMC.exists(kBackupPath);
  if (!sdReady) return;
  if (SD_MMC.exists(kConfigPath)) storageValid = readConfig(kConfigPath, rules, ruleCount, nextId);
  if (!storageValid && SD_MMC.exists(kBackupPath)) storageValid = readConfig(kBackupPath, rules, ruleCount, nextId);
  if (!storageValid) { ruleCount = 0; nextId = 1; }
}

void automaticReplayStop() {
  replayArmed.store(false);
  armPending = false;
  queueCount = 0;
  playingRule.store(-1);
  stopSignal();
  if (playbackStatus().automatic) playbackStop();
}

bool automaticReplayArmed() { return replayArmed.load(); }

bool automaticReplayArmPending() {
  if (armPending && static_cast<uint32_t>(millis() - requestedAt) > 30000) armPending = false;
  return armPending;
}

bool automaticReplayRequestArm(String &error) {
  if (replayArmed.load() || !displayReady() || !storageValid || !appSdReady() || !clockTrusted() || !recorderAdcReady() ||
      !recorderTaskReady() || !playbackStatus().radioReady || fileManagerBusy() ||
      radioProgrammingBusy() || recorderState() != CaptureState::idle ||
      playbackGetState() != PlaybackState::idle || pttTestActive()) {
    error = "Replay prerequisites unavailable"; return false;
  }
  bool active = false;
  for (size_t index = 0; index < ruleCount; ++index) if (rules[index].active && !rules[index].completed) {
    ReplayRule &rule = rules[index];
    uint16_t count = 0;
    uint32_t fingerprint = 0;
    if (rule.folder) {
      if (!folderSnapshot(rule, count, fingerprint, false) || count != rule.fileCount ||
          fingerprint != rule.fingerprint) { error = "Folder changed or unavailable"; return false; }
      active = true;
      continue;
    }
    File file = SD_MMC.open(rule.path, FILE_READ);
    uint32_t remaining = 0;
    bool valid = file && !file.isDirectory() && findPcmData(file, remaining);
    if (file) file.close();
    if (!valid) { error = "Missing or invalid active WAV"; return false; }
    active = true;
  }
  if (!active) { error = "No active rule"; return false; }
  requestedAt = millis();
  armPending = true;
  return true;
}

bool automaticReplayConfirmArm() {
  if (!automaticReplayArmPending() || replayArmed.load()) return false;
  String error;
  armPending = false;
  if (!automaticReplayRequestArm(error)) { replayError = error; return false; }
  armPending = false;
  time_t now = time(nullptr);
  queueCount = 0;
  for (size_t index = 0; index < ruleCount; ++index)
    nextOccurrence[index] = rules[index].active && !rules[index].completed ?
      replayNextOccurrence(rules[index].schedule, static_cast<int64_t>(now), rules[index].id) : UINT32_MAX;
  replayError = "";
  replayArmed.store(true);
  return true;
}

bool automaticReplayReadyToKey() {
  int index = playingRule.load();
  uint32_t now = millis();
  SignalStatus sample = recorderSignalStatus();
  return replayArmed.load() && index >= 0 && index < static_cast<int>(ruleCount) &&
         clockTrusted() && inWindow(rules[index], static_cast<int64_t>(time(nullptr))) &&
         recorderState() == CaptureState::idle && !pttTestActive() && !radioProgrammingBusy() &&
         sample.enabled && sample.active && !sample.fault && sample.sequence &&
         sample.rms < automaticStatus(false).config.threshold && quietSince.load() &&
         static_cast<uint32_t>(now - lastSampleAt.load()) <= 300 &&
         static_cast<uint32_t>(now - quietSince.load()) >= kSilenceMs;
}

void pollAutomaticReplay() {
  automaticReplayArmPending();
  if (!replayArmed.load()) return;
  if (!clockTrusted() || !appSdReady()) {
    replayError = "Clock or SD unavailable"; automaticReplayStop(); return;
  }
  if (playingRule.load() >= 0) {
    if (playbackStatus().automatic) return;
    int index = playingRule.exchange(-1);
    PlaybackStatus finished = playbackStatus();
    stopSignal();
    if (finished.error.length()) { replayError = finished.error; automaticReplayStop(); return; }
    ReplayRule &rule = rules[index];
    if (rule.folder) {
      String basename = String(playingFile).substring(String(playingFile).lastIndexOf('/') + 1);
      basename.toCharArray(rule.cursor, sizeof(rule.cursor));
      if (queueCount && queue[0].remaining) --queue[0].remaining;
      String following;
      ReplayRule remainingInRound = rule;
      remainingInRound.loop = false;
      bool more = folderSelect(remainingInRound, following);
      if (!rule.schedule.intervalSeconds || (!rule.loop && !more)) {
        rule.active = false;
        rule.completed = true;
        if (queueCount) queue[0].remaining = 0;
      } else if (rule.loop && !more) {
        rule.cursor[0] = 0;
        if (rule.shuffled) ++rule.round;
      }
      if (!writeConfig()) { replayError = "Cannot persist folder position"; automaticReplayStop(); return; }
      if (queueCount && !queue[0].remaining) popEvent();
    } else if (!rule.schedule.intervalSeconds) {
      rule.active = false;
      rule.completed = true;
      if (!writeConfig()) { replayError = "Cannot persist completed rule"; automaticReplayStop(); return; }
    }
    return;
  }
  int64_t nowUtc = static_cast<int64_t>(time(nullptr));
  for (size_t index = 0; index < ruleCount; ++index) {
    ReplayRule &rule = rules[index];
    if (!rule.active || rule.completed || nextOccurrence[index] == UINT32_MAX) continue;
    while (nextOccurrence[index] != UINT32_MAX) {
      uint32_t occurrence = nextOccurrence[index];
      int64_t due = replayDueAt(rule.schedule, occurrence, rule.id);
      if (due > nowUtc) break;
      if (rule.schedule.intervalSeconds) ++nextOccurrence[index];
      else nextOccurrence[index] = UINT32_MAX;
      if (!inWindow(rule, due)) continue;
      if (queueCount == kQueueLimit) { replayError = "Replay queue full"; automaticReplayStop(); return; }
      enqueue({static_cast<uint8_t>(index), occurrence, due, static_cast<uint16_t>(rule.folder && rule.all ? rule.fileCount : 1)});
    }
  }
  if (!queueCount) { stopSignal(); return; }
  ReplayEvent event = queue[0];
  ReplayRule &rule = rules[event.ruleIndex];
  if (!rule.active) { popEvent(); stopSignal(); return; }
  if (!inWindow(rule, nowUtc)) { stopSignal(); return; }
  if (recorderState() != CaptureState::idle || fileManagerBusy() || radioProgrammingBusy() ||
      playbackGetState() != PlaybackState::idle || pttTestActive()) { quietSince.store(0); return; }
  if (!recorderSignalStatus().enabled) {
    recorderSetSignal(true);
    signalOwned = recorderSignalStatus().enabled;
    quietSince.store(0);
    lastSequence = 0;
    return;
  }
  SignalStatus sample = recorderSignalStatus();
  uint32_t nowMs = millis();
  if (!sample.active || sample.fault || !sample.enabled) {
    if (sample.fault) { replayError = "Radio ADC fault"; automaticReplayStop(); }
    quietSince.store(0); return;
  }
  if (sample.sequence != lastSequence) {
    quietSince.store(replaySilenceSince(quietSince.load(), lastSampleAt.load(), lastSequence,
                     nowMs, sample.sequence, sample.rms, automaticStatus(false).config.threshold));
    lastSequence = sample.sequence;
    lastSampleAt.store(nowMs);
  }
  if (!automaticReplayReadyToKey()) return;
  String selected = rule.path;
  if (rule.folder) {
    uint16_t count = 0;
    uint32_t fingerprint = 0;
    if (!folderSnapshot(rule, count, fingerprint, false) || count != rule.fileCount ||
        fingerprint != rule.fingerprint || !folderSelect(rule, selected)) {
      replayError = "Replay folder changed or exhausted"; automaticReplayStop(); return;
    }
  }
  File file = SD_MMC.open(selected, FILE_READ);
  uint32_t remaining = 0;
  bool valid = file && !file.isDirectory() && findPcmData(file, remaining);
  if (file) file.close();
  if (!valid) { replayError = "Replay WAV missing or invalid"; automaticReplayStop(); return; }
  if (!appCanStartPlayback()) { quietSince.store(0); return; }
  playingRule.store(event.ruleIndex);
  selected.toCharArray(playingFile, sizeof(playingFile));
  if (!rule.folder) popEvent(false);
  playbackStart(selected, PlaybackTarget::radio, true);
  if (!playbackStatus().automatic) { replayError = "Replay start failed"; automaticReplayStop(); }
}

bool automaticReplaySaveRule(const String &json, String &error) {
  if (!storageValid || !appSdReady() || fileManagerBusy() || radioProgrammingBusy() ||
      recorderState() != CaptureState::idle || playbackGetState() != PlaybackState::idle ||
      (replayArmed.load() || automaticReplayArmPending()) || json.length() > 2048) {
    error = "SD or replay busy"; return false;
  }
  cJSON *input = cJSON_Parse(json.c_str());
  if (!input) { error = "Invalid rule JSON"; return false; }
  if (!cJSON_IsObject(input)) { cJSON_Delete(input); error = "Invalid rule JSON"; return false; }
  for (const cJSON *field = input->child; field; field = field->next)
    for (const cJSON *other = field->next; other; other = other->next)
      if (!field->string || !other->string || !strcmp(field->string, other->string)) {
        cJSON_Delete(input); error = "Duplicate rule field"; return false;
      }
  int64_t id = 0;
  const cJSON *providedId = cJSON_GetObjectItemCaseSensitive(input, "id");
  bool hasId = providedId && integerField(input, "id", 1, UINT32_MAX - 1, id);
  if (providedId && !hasId) { cJSON_Delete(input); error = "Invalid rule id"; return false; }
  if (cJSON_GetObjectItemCaseSensitive(input, "delaySeconds")) {
    int64_t delay = 0, interval = 0, anchor = 0;
    if (!clockTrusted() || cJSON_GetObjectItemCaseSensitive(input, "anchorUtc") ||
        !integerField(input, "intervalSeconds", 0, 0, interval) ||
        !integerField(input, "delaySeconds", 1, 31536000, delay) ||
        !replayResolveDelay(static_cast<int64_t>(time(nullptr)), static_cast<uint32_t>(delay), anchor)) {
      cJSON_Delete(input); error = "Invalid relative delay or clock"; return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(input, "delaySeconds");
    cJSON_AddNumberToObject(input, "anchorUtc", static_cast<double>(anchor));
  }
  ReplayRule candidate;
  cJSON *normalized = cJSON_Duplicate(input, 1);
  cJSON_Delete(input);
  if (!normalized) { error = "Out of memory"; return false; }
  cJSON_DeleteItemFromObjectCaseSensitive(normalized, "active");
  cJSON_DeleteItemFromObjectCaseSensitive(normalized, "completed");
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(normalized, "folder"))) {
    cJSON_DeleteItemFromObjectCaseSensitive(normalized, "cursor");
    cJSON_DeleteItemFromObjectCaseSensitive(normalized, "fileCount");
    cJSON_DeleteItemFromObjectCaseSensitive(normalized, "fingerprint");
    cJSON_DeleteItemFromObjectCaseSensitive(normalized, "round");
    cJSON_AddStringToObject(normalized, "cursor", "");
    cJSON_AddNumberToObject(normalized, "fileCount", 0);
    cJSON_AddNumberToObject(normalized, "fingerprint", 0);
    cJSON_AddNumberToObject(normalized, "round", 0);
  }
  if (!providedId) cJSON_AddNumberToObject(normalized, "id", nextId);
  cJSON_AddBoolToObject(normalized, "active", false);
  cJSON_AddBoolToObject(normalized, "completed", false);
  bool valid = decodeRule(normalized, candidate);
  cJSON_Delete(normalized);
  if (!valid) { error = "Invalid file path or schedule"; return false; }
  size_t index = 0;
  if (hasId) {
    for (; index < ruleCount && rules[index].id != candidate.id; ++index) {}
    if (index == ruleCount) { error = "Rule not found"; return false; }
    if (rules[index].active) { error = "Deactivate rule first"; return false; }
  } else {
    if (ruleCount == kMaxRules || nextId == UINT32_MAX) { error = "Rule limit reached"; return false; }
    index = ruleCount++;
    ++nextId;
  }
  ReplayRule previous = rules[index];
  rules[index] = candidate;
  if (!writeConfig()) {
    rules[index] = previous;
    if (!hasId) { --ruleCount; --nextId; }
    error = "Cannot save SD configuration"; return false;
  }
  storageValid = true;
  return true;
}

bool automaticReplaySetRuleActive(uint32_t id, bool enabled, String &error) {
  if (!storageValid || !appSdReady() || fileManagerBusy() || radioProgrammingBusy() ||
      recorderState() != CaptureState::idle || replayArmed.load() || automaticReplayArmPending() ||
      playbackGetState() != PlaybackState::idle) {
    error = "SD or replay busy"; return false;
  }
  size_t index = 0;
  for (; index < ruleCount && rules[index].id != id; ++index) {}
  if (index == ruleCount) { error = "Rule not found"; return false; }
  ReplayRule &rule = rules[index];
  if (enabled && (rule.completed || !appClockKnown() ||
      (appClockIndicator() != 'R' && appClockIndicator() != 'N' && appClockIndicator() != 'K' && appClockIndicator() != 'T'))) {
    error = "Valid RTC, NTP, manual or phone time required"; return false;
  }
  if (enabled) {
    bool supported = false;
    if (rule.folder) {
      supported = folderSnapshot(rule, rule.fileCount, rule.fingerprint, true);
    } else {
      File file = SD_MMC.open(rule.path, FILE_READ);
      uint32_t remaining = 0;
      supported = file && !file.isDirectory() && findPcmData(file, remaining);
      if (file) file.close();
    }
    if (!supported) { error = "Missing or unsupported WAV"; return false; }
  }
  ReplayRule previous = rule;
  rule.active = enabled;
  if (!writeConfig()) { rule = previous; error = "Cannot save SD configuration"; return false; }
  return true;
}

String automaticReplayRulesJson() {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddBoolToObject(root, "armed", replayArmed.load());
  cJSON_AddBoolToObject(root, "armPending", automaticReplayArmPending());
  cJSON_AddBoolToObject(root, "storageValid", storageValid);
  cJSON_AddNumberToObject(root, "queueLength", queueCount);
  cJSON_AddStringToObject(root, "error", replayError.c_str());
  cJSON *items = cJSON_AddArrayToObject(root, "rules");
  if (items) for (size_t index = 0; index < ruleCount; ++index) {
    cJSON *item = encodeRule(rules[index]);
    if (item) {
      uint32_t next = replayNextOccurrence(rules[index].schedule, static_cast<int64_t>(time(nullptr)), rules[index].id);
      cJSON_AddNumberToObject(item, "nextUtc", next == UINT32_MAX ? 0 :
                              static_cast<double>(replayDueAt(rules[index].schedule, next, rules[index].id)));
      cJSON_AddItemToArray(items, item);
    }
  }
  char *text = cJSON_PrintUnformatted(root);
  String result = text ? String(text) : "{}";
  cJSON_free(text);
  cJSON_Delete(root);
  return result;
}