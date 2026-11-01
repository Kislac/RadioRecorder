#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

struct ManagedFileInfo {
  std::string name;
  bool directory = false;
  uint64_t bytes = 0;
  int64_t modified = 0;
};

inline std::string fileFold(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) { return std::tolower(value); });
  return text;
}

inline bool managedPathValid(const std::string &path, bool allowRoot = true) {
  if (path.empty() || path.size() > 220 || path.front() != '/') return false;
  if (path == "/") return allowRoot;
  size_t start = 1;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    std::string name = path.substr(start, end - start);
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    for (unsigned char value : name) if (value < 32 || value == 127 || std::string("\\:\"<>|?*").find(value) != std::string::npos) return false;
    std::string folded = fileFold(name);
    if (folded.compare(0, 4, ".rr-") == 0) return false;
    std::string stem = folded.substr(0, folded.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    if (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '1' && stem[3] <= '9') return false;
    if (end == path.size()) return true;
    start = end + 1;
  }
  return false;
}

inline bool managedDestinationValid(const std::string &source, const std::string &destination) {
  if (!managedPathValid(source, false) || !managedPathValid(destination, false)) return false;
  std::string from = fileFold(source), to = fileFold(destination);
  return from != to && to.compare(0, from.size() + 1, from + "/") != 0;
}

inline bool managedFileBefore(const ManagedFileInfo &first, const ManagedFileInfo &second,
                              const std::string &sort = "date", bool descending = true) {
  if (first.directory != second.directory) return first.directory;
  if (sort == "date" && first.modified != second.modified) return descending ? first.modified > second.modified : first.modified < second.modified;
  if (sort == "size" && !first.directory && first.bytes != second.bytes) return descending ? first.bytes > second.bytes : first.bytes < second.bytes;
  std::string firstName = fileFold(first.name), secondName = fileFold(second.name);
  if (firstName != secondName) return (sort == "name" || sort == "date") && descending ? firstName > secondName : firstName < secondName;
  return first.name < second.name;
}

inline void managedPageInsert(std::vector<ManagedFileInfo> &page, const ManagedFileInfo &entry,
                              size_t capacity, const std::string &sort, bool descending) {
  auto position = std::lower_bound(page.begin(), page.end(), entry, [&](const ManagedFileInfo &first, const ManagedFileInfo &second) {
    return managedFileBefore(first, second, sort, descending);
  });
  if (page.size() == capacity && position == page.end()) return;
  page.insert(position, entry);
  if (page.size() > capacity) page.pop_back();
}