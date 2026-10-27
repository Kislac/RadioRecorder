#include "wav.h"

void put16(uint8_t *destination, uint16_t value) {
  destination[0] = value & 0xff;
  destination[1] = value >> 8;
}

static void put32(uint8_t *destination, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) destination[shift / 8] = value >> shift;
}

bool writeWavHeader(File &file, uint32_t dataBytes) {
  uint8_t header[44] = {
      'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
      'd', 'a', 't', 'a', 0, 0, 0, 0};
  put32(header + 4, dataBytes + 36);
  put32(header + 24, kSampleRate);
  put32(header + 28, kSampleRate * 2);
  put32(header + 40, dataBytes);
  return file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
}

uint16_t wav16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

static uint32_t wav32(const uint8_t *bytes) {
  return static_cast<uint32_t>(wav16(bytes)) | static_cast<uint32_t>(wav16(bytes + 2)) << 16;
}

bool findPcmData(File &file, uint32_t &remaining) {
  uint8_t header[16];
  if (!file.seek(0) || file.read(header, 12) != 12 || memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) return false;
  uint64_t riffEnd = static_cast<uint64_t>(wav32(header + 4)) + 8;
  if (riffEnd > file.size() || riffEnd > UINT32_MAX || riffEnd < 44) return false;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  bool pcmFormat = false;
  while (static_cast<uint64_t>(file.position()) + 8 <= riffEnd) {
    if (file.read(header, 8) != 8) return false;
    uint32_t size = wav32(header + 4);
    uint64_t end = static_cast<uint64_t>(file.position()) + size + (size & 1);
    if (end > riffEnd) return false;
    if (memcmp(header, "fmt ", 4) == 0 && size >= 16) {
      if (file.read(header, 16) != 16) return false;
      pcmFormat = wav16(header) == 1 && wav16(header + 2) == 1 && wav32(header + 4) == kSampleRate &&
                  wav32(header + 8) == kSampleRate * 2 && wav16(header + 12) == 2 && wav16(header + 14) == 16;
    } else if (memcmp(header, "data", 4) == 0) {
      dataOffset = file.position();
      dataSize = size;
    }
    if (!file.seek(static_cast<uint32_t>(end))) return false;
  }
  if (!pcmFormat || !dataOffset || !dataSize || (dataSize & 1)) return false;
  remaining = dataSize;
  return file.seek(dataOffset);
}