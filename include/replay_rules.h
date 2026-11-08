#pragma once

#include <cstdint>
#include <cstring>

enum class ReplayWindow : uint8_t { any, active, quiet };

struct ReplaySchedule {
  int64_t anchorUtc = 0;
  uint32_t intervalSeconds = 0;
  uint16_t jitterMinutes = 0;
  ReplayWindow window = ReplayWindow::any;
  uint16_t startMinute = 0;
  uint16_t endMinute = 0;
};

inline bool replayScheduleValid(const ReplaySchedule &rule) {
  return rule.anchorUtc >= 1577836800 && rule.anchorUtc <= 4102444800LL &&
         rule.intervalSeconds <= 604800 &&
         (rule.intervalSeconds == 0 ? rule.jitterMinutes == 0 :
          static_cast<uint32_t>(rule.jitterMinutes) * 120 < rule.intervalSeconds) &&
         rule.startMinute < 1440 && rule.endMinute < 1440 &&
         (rule.window == ReplayWindow::any ||
          ((rule.window == ReplayWindow::active || rule.window == ReplayWindow::quiet) &&
           rule.startMinute != rule.endMinute));
}

inline bool replayWindowContains(uint16_t minute, const ReplaySchedule &rule) {
  if (rule.window == ReplayWindow::any) return true;
  bool inside = rule.startMinute < rule.endMinute ?
    minute >= rule.startMinute && minute < rule.endMinute :
    minute >= rule.startMinute || minute < rule.endMinute;
  return rule.window == ReplayWindow::active ? inside : !inside;
}

inline int64_t replayDueAt(const ReplaySchedule &rule, uint32_t occurrence, uint32_t id) {
  if (!rule.intervalSeconds) return occurrence == 0 ? rule.anchorUtc : 0;
  if (!occurrence) return 0;
  int64_t due = rule.anchorUtc + static_cast<int64_t>(occurrence) * rule.intervalSeconds;
  if (rule.jitterMinutes) {
    uint32_t random = (id * 1664525u + occurrence * 1013904223u) ^ 0x9e3779b9u;
    uint32_t spread = static_cast<uint32_t>(rule.jitterMinutes) * 2 + 1;
    due += (static_cast<int64_t>(random % spread) - rule.jitterMinutes) * 60;
  }
  return due;
}

inline uint32_t replayNextOccurrence(const ReplaySchedule &rule, int64_t nowUtc, uint32_t id) {
  if (!rule.intervalSeconds) return rule.anchorUtc > nowUtc ? 0 : UINT32_MAX;
  if (nowUtc < rule.anchorUtc) return 1;
  uint64_t candidate = static_cast<uint64_t>(nowUtc - rule.anchorUtc) / rule.intervalSeconds;
  if (candidate > UINT32_MAX - 2) return UINT32_MAX;
  uint32_t occurrence = candidate > 1 ? static_cast<uint32_t>(candidate - 1) : 1;
  while (replayDueAt(rule, occurrence, id) <= nowUtc) {
    if (occurrence == UINT32_MAX - 1) return UINT32_MAX;
    ++occurrence;
  }
  return occurrence;
}

inline uint32_t replaySilenceSince(uint32_t quietSince, uint32_t lastSampleAt, uint32_t lastSequence,
                                   uint32_t sampledAt, uint32_t sequence, uint16_t rms, uint16_t threshold) {
  if (!sequence || sequence == lastSequence) return quietSince;
  if (rms >= threshold) return 0;
  if (!quietSince || !lastSequence || sequence != lastSequence + 1 ||
      static_cast<uint32_t>(sampledAt - lastSampleAt) > 300) return sampledAt;
  return quietSince;
}

inline bool replayResolveDelay(int64_t nowUtc, uint32_t delaySeconds, int64_t &anchorUtc) {
  if (nowUtc < 1577836800 || !delaySeconds || delaySeconds > 31536000 ||
      nowUtc + delaySeconds > 4102444800LL) return false;
  anchorUtc = nowUtc + delaySeconds;
  return true;
}

inline uint32_t replayFileHash(const char *name, uint32_t id) {
  uint32_t hash = 2166136261u ^ id;
  for (const unsigned char *letter = reinterpret_cast<const unsigned char *>(name); *letter; ++letter)
    hash = (hash ^ *letter) * 16777619u;
  return hash;
}

inline bool replayFileBefore(const char *first, const char *second, bool shuffled, uint32_t id) {
  if (shuffled) {
    uint32_t left = replayFileHash(first, id), right = replayFileHash(second, id);
    if (left != right) return left < right;
  }
  return strcmp(first, second) < 0;
}

inline bool replayFileAfterCursor(const char *candidate, const char *cursor, const char *best,
                                 bool shuffled, uint32_t seed) {
  return (!*cursor || replayFileBefore(cursor, candidate, shuffled, seed)) &&
         (!*best || replayFileBefore(candidate, best, shuffled, seed));
}