#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

struct ManagedZipTotals {
  uint64_t archiveBytes = 22;
  uint32_t entries = 0;
  bool add(size_t nameBytes, uint32_t fileBytes) {
    archiveBytes += 30ULL + 16 + 46 + 2 * nameBytes + fileBytes;
    return ++entries <= 65535 && archiveBytes <= 0xffffffffULL;
  }
};

inline uint32_t managedZipCrc(uint32_t crc, const uint8_t *buffer, size_t size) {
  for (size_t index = 0; index < size; ++index) {
    crc ^= buffer[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320U : 0);
  }
  return crc;
}

template<class Output>
bool managedZipLocal(Output &output, const std::string &name, uint16_t time = 0, uint16_t date = 33) {
  return output.dword(0x04034b50) && output.word(20) && output.word(0x0808) && output.word(0) &&
         output.word(time) && output.word(date) && output.dword(0) && output.dword(0) && output.dword(0) &&
         output.word(name.size()) && output.word(0) && output.text(name);
}

template<class Output>
bool managedZipDescriptor(Output &output, uint32_t crc, uint32_t size) {
  return output.dword(0x08074b50) && output.dword(crc) && output.dword(size) && output.dword(size);
}

template<class Output>
bool managedZipCentral(Output &output, const std::string &name, uint32_t crc, uint32_t size, uint32_t offset,
                       bool directory, uint16_t time = 0, uint16_t date = 33) {
  return output.dword(0x02014b50) && output.word(20) && output.word(20) && output.word(0x0808) && output.word(0) &&
         output.word(time) && output.word(date) && output.dword(crc) && output.dword(size) && output.dword(size) &&
         output.word(name.size()) && output.word(0) && output.word(0) && output.word(0) && output.word(0) &&
         output.dword(directory ? 16 : 0) && output.dword(offset) && output.text(name);
}

template<class Output>
bool managedZipEnd(Output &output, uint16_t entries, uint32_t centralBytes, uint32_t offset) {
  return output.dword(0x06054b50) && output.word(0) && output.word(0) && output.word(entries) && output.word(entries) &&
         output.dword(centralBytes) && output.dword(offset) && output.word(0);
}