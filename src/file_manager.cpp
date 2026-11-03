#include "file_manager.h"
#include "zip_format.h"
#include "automatic_record.h"
#include "app_control.h"
#include "playback.h"
#include "recorder.h"
#include "watchdog.h"
#include "wifi_manager.h"
#include "performance.h"
#include <SD_MMC.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <atomic>
#include <sys/stat.h>
#include <utime.h>

namespace {
WiFiServer transferServer(82);
WiFiClient pendingClient;
String pendingHeaders;
uint32_t pendingStarted = 0;
SemaphoreHandle_t jobMutex = nullptr;
uint32_t nextJob = 0;
uint8_t transferBuffer[4096];
std::atomic<uint32_t> lastWorkerStackFree{0};

struct FileJob {
  uint32_t id = 0;
  bool active = false;
  bool cancel = false;
  String action, source, destination, state = "idle", error;
  uint64_t totalBytes = 0, completedBytes = 0, sentBytes = 0, archiveBytes = 0;
  uint32_t totalFiles = 0, completedFiles = 0;
  WiFiClient client;
};
FileJob job;

struct FileRecord {
  FileRecord *next = nullptr;
  char relative[221] = {};
  bool directory = false;
  uint32_t bytes = 0, crc = 0, offset = 0;
  time_t modified = 0;
};

void ensureMutex() { if (!jobMutex) jobMutex = xSemaphoreCreateMutex(); }
void lockJob() { xSemaphoreTake(jobMutex, portMAX_DELAY); }
void unlockJob() { xSemaphoreGive(jobMutex); }
void tick() {
  watchdogResetCurrentTask();
  static uint32_t lastYield = 0;
  if (millis() - lastYield >= 5) { vTaskDelay(1); lastYield = millis(); }
}
bool cancelled() { lockJob(); bool value = job.cancel; unlockJob(); return value; }
void state(const char *value) { lockJob(); job.state = value; unlockJob(); }
void progress(uint64_t bytes, uint32_t files = 0) { lockJob(); job.completedBytes = bytes; job.completedFiles = files; unlockJob(); }

String baseName(const File &file) {
  String name = file.name();
  return name.substring(name.lastIndexOf('/') + 1);
}

int64_t modifiedTime(File &file) {
  time_t timestamp = file.getLastWrite();
  if (timestamp >= 946684800) return timestamp;
  String name = baseName(file);
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (name.length() >= 10 && name[4] == '-' && name[7] == '-') {
    if (sscanf(name.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3) return 0;
  } else {
    int separator = name.indexOf('_');
    if (separator < 6 || sscanf(name.c_str() + separator - 6, "%2d%2d%2d_%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    year += 2000;
  }
  if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
  tm local = {}; local.tm_year = year - 1900; local.tm_mon = month - 1; local.tm_mday = day;
  local.tm_hour = hour; local.tm_min = minute; local.tm_sec = second; local.tm_isdst = -1;
  return mktime(&local);
}

bool mutationReady(String &error) {
  if (!appSdReady() || recorderState() != CaptureState::idle || playbackGetState() != PlaybackState::idle || fileManagerBusy()) {
    error = "SD, recorder, playback or file operation busy"; return false;
  }
  return true;
}

bool parentExists(const String &path) {
  int separator = path.lastIndexOf('/');
  File parent = SD_MMC.open(separator == 0 ? "/" : path.substring(0, separator), FILE_READ);
  bool valid = parent && parent.isDirectory(); parent.close(); return valid;
}

bool removeTree(const String &path, uint8_t depth = 0, bool allowCancel = false) {
  if (depth > 16) return false;
  while (true) {
    if (allowCancel && cancelled()) return false;
    File directory = SD_MMC.open(path, FILE_READ);
    if (!directory) return !SD_MMC.exists(path);
    if (!directory.isDirectory()) { directory.close(); return SD_MMC.remove(path); }
    File child = directory.openNextFile();
    if (!child) { directory.close(); return SD_MMC.rmdir(path); }
    String childPath = path + "/" + baseName(child);
    child.close(); directory.close();
    if (!removeTree(childPath, depth + 1, allowCancel)) return false;
    tick();
  }
}

bool collect(const String &root, const String &relative, FileRecord *&head, FileRecord *&tail, uint8_t depth,
             uint64_t &bytes, uint32_t &files, const String &destination) {
  if (depth > 16 || relative.length() > 220 || cancelled()) return false;
  String path = relative.isEmpty() ? root : root + "/" + relative;
  if (!managedPathValid(path.c_str(), false)) return false;
  if (!destination.isEmpty() && !managedPathValid((relative.isEmpty() ? destination : destination + "/" + relative).c_str(), false)) return false;
  File entry = SD_MMC.open(path, FILE_READ);
  if (!entry) return false;
  auto *record = static_cast<FileRecord *>(heap_caps_calloc(1, sizeof(FileRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!record) record = static_cast<FileRecord *>(calloc(1, sizeof(FileRecord)));
  if (!record) { entry.close(); return false; }
  snprintf(record->relative, sizeof(record->relative), "%s", relative.c_str());
  record->directory = entry.isDirectory(); record->bytes = record->directory ? 0 : entry.size();
  record->modified = entry.getLastWrite();
  if (tail) tail->next = record; else head = record;
  tail = record; bytes += record->bytes;
  if (!record->directory) ++files;
  lockJob(); job.totalBytes = bytes; job.totalFiles = files; unlockJob();
  entry.close();
  if (record->directory) {
    uint32_t childIndex = 0;
    while (true) {
      File directory = SD_MMC.open(path, FILE_READ), child;
      if (!directory) return false;
      for (uint32_t index = 0; index < childIndex; ++index) { child = directory.openNextFile(); child.close(); }
      child = directory.openNextFile();
      if (!child) { directory.close(); break; }
      ++childIndex;
      String name = baseName(child); child.close(); directory.close();
      if (name.startsWith(".rr-")) continue;
      if (!collect(root, relative.isEmpty() ? name : relative + "/" + name, head, tail, depth + 1, bytes, files, destination)) {
        return false;
      }
      tick();
    }
  }
  entry.close(); tick(); return true;
}

uint32_t crcUpdate(uint32_t crc, const uint8_t *buffer, size_t size) {
  return managedZipCrc(crc, buffer, size);
}

bool verifyFile(const String &path, uint64_t expected, uint32_t crc) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.size() != expected) { file.close(); return false; }
  uint32_t actual = 0xffffffffU;
  while (file.available()) {
    if (cancelled()) { file.close(); return false; }
    size_t count = file.read(transferBuffer, sizeof(transferBuffer));
    if (!count) { file.close(); return false; }
    actual = crcUpdate(actual, transferBuffer, count); tick();
  }
  file.close(); return ~actual == crc;
}

bool sendBytes(WiFiClient &client, const uint8_t *bytes, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    if (cancelled() || !client.connected()) return false;
    size_t count = client.write(bytes + offset, length - offset);
    if (!count) return false;
    offset += count;
    lockJob(); job.sentBytes += count; unlockJob(); tick();
  }
  return true;
}

bool send16(WiFiClient &client, uint16_t value) {
  uint8_t bytes[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return sendBytes(client, bytes, sizeof(bytes));
}
bool send32(WiFiClient &client, uint32_t value) {
  return send16(client, value) && send16(client, value >> 16);
}

struct ZipOutput {
  WiFiClient &client;
  bool word(uint16_t value) { return send16(client, value); }
  bool dword(uint32_t value) { return send32(client, value); }
  bool text(const std::string &value) { return sendBytes(client, reinterpret_cast<const uint8_t *>(value.data()), value.size()); }
};

void zipTimestamp(const FileRecord &record, uint16_t &time, uint16_t &date) {
  time = 0; date = 33;
  tm local = {};
  if (!localtime_r(&record.modified, &local) || local.tm_year < 80 || local.tm_year > 207) return;
  time = (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2);
  date = ((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday;
}

bool streamZip(FileJob &local, FileRecord *records) {
  String label = local.source.substring(local.source.lastIndexOf('/') + 1);
  ManagedZipTotals totals;
  for (FileRecord *record = records; record; record = record->next) {
    String name = label + (record->relative[0] ? "/" + String(record->relative) : "");
    if (record->directory) name += '/';
    if (!totals.add(name.length(), record->bytes)) {
      lockJob(); job.error = "ZIP exceeds the 4 GiB / 65535 entry limit"; unlockJob(); return false;
    }
  }
  uint64_t archive = totals.archiveBytes; uint32_t entries = totals.entries;
  lockJob(); job.archiveBytes = archive; unlockJob(); state("ready");
  bool subscribed = false;
  uint32_t waitStarted = millis();
  while (!cancelled()) {
    lockJob(); local.client = job.client; unlockJob();
    if (local.client.connected()) break;
    if (millis() - waitStarted > 60000) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (cancelled()) return false;
  subscribed = watchdogSubscribeCurrentTask(); state("running");
  String headers = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Disposition: attachment; filename*=UTF-8''";
  for (uint8_t value : label) {
    char encoded[4]; snprintf(encoded, sizeof(encoded), "%%%02X", value); headers += encoded;
  }
  headers += ".zip\r\nContent-Length: " + String(archive) + "\r\nConnection: close\r\n\r\n";
  if (local.client.print(headers) != headers.length()) {
    local.client.stop(); watchdogUnsubscribeCurrentTask(subscribed); return false;
  }
  ZipOutput output{local.client};
  uint32_t offset = 0, files = 0; uint64_t bytes = 0; bool valid = true;
  for (FileRecord *record = records; record && valid; record = record->next) {
    String name = label + (record->relative[0] ? "/" + String(record->relative) : "");
    if (record->directory) name += '/';
    record->offset = offset;
    uint16_t time, date; zipTimestamp(*record, time, date);
    valid = managedZipLocal(output, name.c_str(), time, date);
    uint32_t crc = 0xffffffffU;
    if (!record->directory && valid) {
      String path = record->relative[0] ? local.source + "/" + record->relative : local.source;
      File file = SD_MMC.open(path, FILE_READ);
      if (!file || file.size() != record->bytes) valid = false;
      uint32_t copied = 0;
      while (valid && copied < record->bytes) {
        size_t count = file.read(transferBuffer, std::min(sizeof(transferBuffer), static_cast<size_t>(record->bytes - copied)));
        if (!count || !sendBytes(local.client, transferBuffer, count)) { valid = false; break; }
        crc = crcUpdate(crc, transferBuffer, count); copied += count; bytes += count; progress(bytes, files);
      }
      file.close(); ++files;
    }
    record->crc = ~crc;
    valid = valid && managedZipDescriptor(output, record->crc, record->bytes);
    offset += 30 + name.length() + record->bytes + 16; progress(bytes, files);
  }
  uint32_t centralStart = offset;
  for (FileRecord *record = records; record && valid; record = record->next) {
    String name = label + (record->relative[0] ? "/" + String(record->relative) : "");
    if (record->directory) name += '/';
    uint16_t time, date; zipTimestamp(*record, time, date);
    valid = managedZipCentral(output, name.c_str(), record->crc, record->bytes, record->offset, record->directory, time, date);
    offset += 46 + name.length();
  }
  valid = valid && managedZipEnd(output, entries, offset - centralStart, centralStart);
  local.client.stop(); watchdogUnsubscribeCurrentTask(subscribed); return valid;
}

bool copyRecords(FileJob &local, FileRecord *records, const String &staging, bool &created) {
  uint64_t bytes = 0; uint32_t files = 0;
  for (FileRecord *record = records; record; record = record->next) {
    if (cancelled()) return false;
    String from = record->relative[0] ? local.source + "/" + record->relative : local.source;
    String to = record->relative[0] ? staging + "/" + record->relative : staging;
    if (record->directory) { if (!SD_MMC.mkdir(to)) return false; if (!record->relative[0]) created = true; continue; }
    File source = SD_MMC.open(from, FILE_READ);
    if (!source) return false;
    File target = SD_MMC.open(to, FILE_WRITE);
    if (!record->relative[0] && target) created = true;
    if (!source || !target) { source.close(); target.close(); return false; }
    uint32_t crc = 0xffffffffU, copied = 0;
    while (copied < record->bytes) {
      if (cancelled()) { source.close(); target.close(); return false; }
      size_t count = source.read(transferBuffer, std::min(sizeof(transferBuffer), static_cast<size_t>(record->bytes - copied)));
      if (!count || target.write(transferBuffer, count) != count) { source.close(); target.close(); return false; }
      crc = crcUpdate(crc, transferBuffer, count); copied += count; bytes += count; progress(bytes, files); tick();
    }
    source.close(); target.close(); state("verifying");
    if (!verifyFile(to, record->bytes, ~crc)) return false;
    if (record->modified >= 946684800) {
      utimbuf times = {record->modified, record->modified}; utime(("/sdcard" + to).c_str(), &times);
    }
    state("running"); progress(bytes, ++files);
  }
  return true;
}

bool receiveUpload(FileJob &local, const String &staging, bool &created) {
  state("ready"); uint32_t started = millis();
  while (!cancelled()) {
    lockJob(); local.client = job.client; unlockJob();
    if (local.client.connected()) break;
    if (millis() - started > 60000) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (cancelled()) return false;
  bool subscribed = watchdogSubscribeCurrentTask(); state("running");
  File target = SD_MMC.open(staging, FILE_WRITE);
  created = static_cast<bool>(target);
  uint64_t bytes = 0; uint32_t crc = 0xffffffffU, lastData = millis(); bool valid = static_cast<bool>(target);
  while (valid && bytes < local.totalBytes) {
    if (cancelled() || !local.client.connected() || millis() - lastData > 15000) { valid = false; break; }
    size_t available = local.client.available();
    if (available) {
      int received = local.client.read(transferBuffer, std::min(std::min(available, sizeof(transferBuffer)), static_cast<size_t>(local.totalBytes - bytes)));
      if (received <= 0) { valid = false; break; }
      size_t count = static_cast<size_t>(received);
      if (target.write(transferBuffer, count) != count) { valid = false; break; }
      crc = crcUpdate(crc, transferBuffer, count); bytes += count; lastData = millis(); progress(bytes);
    }
    tick();
  }
  target.close();
  if (valid) { state("verifying"); valid = verifyFile(staging, local.totalBytes, ~crc); }
  if (valid) valid = !cancelled() && !SD_MMC.exists(local.destination) && SD_MMC.rename(staging, local.destination);
  local.client.print(String("HTTP/1.1 ") + (valid ? "200 OK" : "500 Transfer failed") + "\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  local.client.stop(); watchdogUnsubscribeCurrentTask(subscribed); return valid;
}

void executeJob() {
  lockJob(); FileJob local = job; unlockJob();
  FileRecord *records = nullptr, *tail = nullptr;
  bool created = false;
  uint64_t bytes = local.totalBytes; uint32_t files = local.action == "upload" ? 1 : 0;
  bool subscribed = watchdogSubscribeCurrentTask();
  bool valid = local.action == "upload" || collect(local.source, "", records, tail, 0, bytes, files, local.destination);
  lockJob(); job.totalBytes = bytes; job.totalFiles = files; unlockJob();
  String staging = local.destination.substring(0, local.destination.lastIndexOf('/') + 1) + ".rr-job-" + String(local.id) + "-" + String(esp_random(), HEX) + ".tmp";
  local.totalBytes = bytes;
  if (valid && (local.action == "copy" || local.action == "upload")) {
    if (SD_MMC.totalBytes() - SD_MMC.usedBytes() < bytes + 65536ULL || SD_MMC.exists(staging)) valid = false;
  }
  if (valid && local.action == "zip") {
    watchdogUnsubscribeCurrentTask(subscribed); subscribed = false; valid = streamZip(local, records);
  } else if (valid && local.action == "upload") {
    watchdogUnsubscribeCurrentTask(subscribed); subscribed = false; valid = receiveUpload(local, staging, created);
  } else if (valid && local.action == "copy") {
    state("running"); valid = copyRecords(local, records, staging, created);
    if (valid) valid = !cancelled() && !SD_MMC.exists(local.destination) && SD_MMC.rename(staging, local.destination);
  } else if (valid && local.action == "move") {
    state("running"); valid = !cancelled() && !SD_MMC.exists(local.destination) && SD_MMC.rename(local.source, local.destination);
    if (valid) progress(bytes, files);
  } else if (valid && local.action == "delete") {
    state("running"); uint64_t removed = 0; uint32_t deleted = 0;
    for (FileRecord *record = records; record && valid; record = record->next) {
      if (cancelled()) { valid = false; break; }
      if (record->directory) continue;
      String path = record->relative[0] ? local.source + "/" + record->relative : local.source;
      valid = SD_MMC.remove(path); removed += record->bytes; progress(removed, ++deleted); tick();
    }
    if (valid && records && records->directory) valid = removeTree(local.source, 0, true);
  }
  if (!valid && created && SD_MMC.exists(staging)) {
    bool cleanupSubscribed = watchdogSubscribeCurrentTask();
    bool cleaned = removeTree(staging); watchdogUnsubscribeCurrentTask(cleanupSubscribed);
    if (!cleaned) { lockJob(); job.error = "Operation failed; temporary data cleanup failed"; unlockJob(); }
  }
  while (records) { FileRecord *next = records->next; free(records); records = next; }
  watchdogUnsubscribeCurrentTask(subscribed);
  lockJob();
  bool wasCancelled = job.cancel;
  job.active = false; job.state = valid ? "complete" : wasCancelled ? "cancelled" : "failed";
  if (!valid && job.error.isEmpty()) job.error = wasCancelled ?
    (local.action == "delete" ? "Cancelled; deletion may be partial" : "Cancelled") :
    "Operation failed: SD, space, memory, path depth or connection error";
  unlockJob();
}

void runJob(void *) {
  executeJob();
  if (performanceEnabled()) lastWorkerStackFree.store(uxTaskGetStackHighWaterMark(nullptr));
  vTaskDelete(nullptr);
}
}

bool fileManagerBusy() {
  if (!jobMutex) return false;
  lockJob(); bool active = job.active; unlockJob(); return active;
}

bool fileManagerList(const String &directoryPath, const String &sort, bool descending, const String &filter,
                     const String &after, uint8_t limit, const String &revision, ManagedFilePage &page, bool wavOnly) {
  page = ManagedFilePage();
  if (!appSdReady() || recorderState() != CaptureState::idle) { page.error = "SD unavailable or recording in progress"; return false; }
  if (!managedPathValid(directoryPath.c_str()) || (sort != "date" && sort != "name" && sort != "size") || !limit || limit > 20) {
    page.error = "Invalid directory or sort"; return false;
  }
  ManagedFileInfo cursor; bool hasCursor = !after.isEmpty();
  if (hasCursor) {
    if (after.indexOf('/') >= 0 || !managedPathValid((directoryPath == "/" ? "/" + after : directoryPath + "/" + after).c_str(), false)) {
      page.error = "Invalid cursor"; return false;
    }
    File previous = SD_MMC.open(directoryPath == "/" ? "/" + after : directoryPath + "/" + after, FILE_READ);
    if (!previous) { page.error = "Directory changed; refresh the list"; return false; }
    cursor.name = after.c_str(); cursor.directory = previous.isDirectory(); cursor.bytes = cursor.directory ? 0 : previous.size(); cursor.modified = modifiedTime(previous); previous.close();
  }
  File directory = SD_MMC.open(directoryPath, FILE_READ);
  if (!directory || !directory.isDirectory()) { directory.close(); page.error = "Directory not found"; return false; }
  uint32_t hash = 0, lastYield = millis(); File entry;
  while ((entry = directory.openNextFile())) {
    pollAutomaticRecording();
    if (recorderState() != CaptureState::idle) {
      entry.close(); directory.close(); page.items.clear(); page.error = "SD unavailable or recording in progress"; return false;
    }
    ManagedFileInfo item; String name = baseName(entry); item.name = name.c_str(); item.directory = entry.isDirectory();
    item.bytes = item.directory ? 0 : entry.size(); item.modified = modifiedTime(entry); entry.close();
    if (name.startsWith(".rr-")) continue;
    uint32_t fingerprint = 2166136261U;
    for (uint8_t value : name) fingerprint = (fingerprint ^ value) * 16777619U;
    fingerprint ^= static_cast<uint32_t>(item.modified) ^ static_cast<uint32_t>(item.bytes) ^ (item.directory ? 0x80000000U : 0);
    hash += fingerprint;
    String lower = name; lower.toLowerCase(); String query = filter; query.toLowerCase();
    if ((wavOnly && !item.directory && !lower.endsWith(".wav")) || (!query.isEmpty() && lower.indexOf(query) < 0)) continue;
    ++page.total;
    if (!hasCursor || managedFileBefore(cursor, item, sort.c_str(), descending)) managedPageInsert(page.items, item, limit + 1, sort.c_str(), descending);
    watchdogResetCurrentTask();
    if (millis() - lastYield >= 5) { delay(1); lastYield = millis(); }
  }
  directory.close(); page.revision = String(hash, HEX) + "-" + String(page.total);
  if (!revision.isEmpty() && revision != page.revision) { page.items.clear(); page.error = "Directory changed; refresh the list"; return false; }
  if (page.items.size() > limit) { page.items.pop_back(); page.next = page.items.back().name.c_str(); }
  return true;
}

bool fileManagerListOffset(const String &directory, uint32_t offset, uint8_t limit, ManagedFilePage &page) {
  String after;
  while (offset) {
    uint8_t chunk = std::min<uint32_t>(offset, 20);
    if (!fileManagerList(directory, "date", true, "", after, chunk, "", page)) return false;
    if (page.items.size() < chunk) { page.items.clear(); return true; }
    after = page.items.back().name.c_str(); offset -= chunk;
  }
  return fileManagerList(directory, "date", true, "", after, limit, "", page);
}

bool fileManagerMkdir(const String &path, String &error) {
  if (!mutationReady(error)) return false;
  if (!managedPathValid(path.c_str(), false) || !parentExists(path)) { error = "Invalid folder path"; return false; }
  if (SD_MMC.exists(path)) { error = "Destination already exists"; return false; }
  if (!SD_MMC.mkdir(path)) { error = "Cannot create folder"; return false; }
  return true;
}

bool fileManagerRename(const String &source, const String &destination, String &error) {
  if (!mutationReady(error)) return false;
  if (!managedDestinationValid(source.c_str(), destination.c_str()) || !parentExists(destination)) { error = "Invalid source or destination"; return false; }
  if (!SD_MMC.exists(source)) { error = "Source not found"; return false; }
  if (SD_MMC.exists(destination)) { error = "Destination already exists"; return false; }
  if (!SD_MMC.rename(source, destination)) { error = "Cannot rename file or folder"; return false; }
  return true;
}

bool fileManagerDeleteFile(const String &path, String &error) {
  if (!mutationReady(error)) return false;
  if (!managedPathValid(path.c_str(), false)) { error = "Invalid file path"; return false; }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) { file.close(); error = "File not found"; return false; }
  file.close(); if (!SD_MMC.remove(path)) { error = "Cannot delete file"; return false; }
  return true;
}

uint32_t fileManagerStartJob(const String &action, const String &source, const String &destination, uint64_t bytes, String &error) {
  ensureMutex();
  if (!jobMutex || !mutationReady(error)) return 0;
  if (action != "copy" && action != "move" && action != "delete" && action != "zip" && action != "upload") { error = "Invalid operation"; return 0; }
  if (action != "upload" && (!managedPathValid(source.c_str(), false) || !SD_MMC.exists(source))) { error = "Source not found or invalid"; return 0; }
  if (action == "copy" || action == "move" || action == "upload") {
    if (!managedPathValid(destination.c_str(), false) || !parentExists(destination) ||
        (action != "upload" && !managedDestinationValid(source.c_str(), destination.c_str()))) { error = "Invalid destination"; return 0; }
    if (SD_MMC.exists(destination)) { error = "Destination already exists"; return 0; }
  }
  if (action == "upload" && bytes > 0xffffffffULL) { error = "File too large"; return 0; }
  lockJob(); job = FileJob(); job.id = ++nextJob; job.active = true; job.action = action;
  job.source = source; job.destination = destination; job.totalBytes = bytes; job.state = "preparing"; uint32_t id = job.id; unlockJob();
  if (xTaskCreatePinnedToCore(runJob, "file-manager", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    lockJob(); job.active = false; job.state = "failed"; job.error = "Cannot start file worker"; unlockJob(); error = "Cannot start file worker"; return 0;
  }
  return id;
}

String fileManagerJobJson(uint32_t id) {
  ensureMutex(); if (!jobMutex) return "{}";
  lockJob(); FileJob current = job; unlockJob();
  if (id && id != current.id) return "{}";
  cJSON *json = cJSON_CreateObject(); if (!json) return "{}";
  cJSON_AddNumberToObject(json, "id", current.id); cJSON_AddStringToObject(json, "action", current.action.c_str());
  cJSON_AddStringToObject(json, "state", current.state.c_str()); cJSON_AddStringToObject(json, "error", current.error.c_str());
  cJSON_AddNumberToObject(json, "totalBytes", current.totalBytes); cJSON_AddNumberToObject(json, "completedBytes", current.completedBytes);
  cJSON_AddNumberToObject(json, "sentBytes", current.sentBytes); cJSON_AddNumberToObject(json, "archiveBytes", current.archiveBytes);
  cJSON_AddNumberToObject(json, "totalFiles", current.totalFiles); cJSON_AddNumberToObject(json, "completedFiles", current.completedFiles);
  cJSON_AddNumberToObject(json, "transferPort", 82);
  char *encoded = cJSON_PrintUnformatted(json); String result = encoded ? encoded : "{}";
  if (encoded) cJSON_free(encoded);
  cJSON_Delete(json);
  return result;
}

bool fileManagerCancelJob(uint32_t id) {
  if (!jobMutex) return false;
  lockJob(); bool valid = job.active && job.id == id; if (valid) job.cancel = true; unlockJob(); return valid;
}

void fileManagerStartServices() { ensureMutex(); transferServer.begin(); }
void fileManagerStopServices() {
  transferServer.end(); pendingClient.stop(); pendingHeaders = "";
  if (jobMutex) { lockJob(); if (job.active) job.cancel = true; unlockJob(); }
}

void pollFileManager() {
  if (fileManagerBusy()) wifiManagerNotifyActivity();
  if (!pendingClient) {
    pendingClient = transferServer.accept();
    if (pendingClient) { pendingStarted = millis(); pendingHeaders = ""; }
  }
  if (!pendingClient) return;
  while (pendingClient.available() && pendingHeaders.length() < 2048 && !pendingHeaders.endsWith("\r\n\r\n")) pendingHeaders += static_cast<char>(pendingClient.read());
  if (!pendingHeaders.endsWith("\r\n\r\n")) {
    if (pendingHeaders.length() >= 2048 || millis() - pendingStarted > 3000) { pendingClient.stop(); pendingHeaders = ""; }
    return;
  }
  if (pendingHeaders.startsWith("OPTIONS ")) {
    pendingClient.print("HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: PUT, GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    pendingClient.stop(); pendingHeaders = ""; return;
  }
  int marker = pendingHeaders.indexOf("?id="), end = pendingHeaders.indexOf(' ', marker);
  uint32_t id = marker >= 0 && end > marker ? pendingHeaders.substring(marker + 4, end).toInt() : 0;
  lockJob();
  bool valid = job.active && job.id == id && job.state == "ready" && !job.client.connected() &&
               ((job.action == "zip" && pendingHeaders.startsWith("GET /zip?id=")) ||
                (job.action == "upload" && pendingHeaders.startsWith("PUT /upload?id=")));
  if (valid && job.action == "upload") {
    String lower = pendingHeaders; lower.toLowerCase(); int lengthAt = lower.indexOf("\r\ncontent-length:");
    uint64_t length = lengthAt >= 0 ? strtoull(lower.c_str() + lengthAt + 17, nullptr, 10) : UINT64_MAX;
    valid = length == job.totalBytes;
  }
  if (valid) { pendingClient.setNoDelay(true); job.client = pendingClient; }
  unlockJob();
  if (!valid) { pendingClient.print("HTTP/1.1 409 Conflict\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); pendingClient.stop(); }
  pendingClient = WiFiClient(); pendingHeaders = "";
}

uint32_t fileManagerLastStackFreeMinimum() { return lastWorkerStackFree.load(); }