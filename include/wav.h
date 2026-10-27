#pragma once

#include <Arduino.h>
#include <FS.h>

constexpr uint32_t kSampleRate = 22050;

void put16(uint8_t *destination, uint16_t value);
uint16_t wav16(const uint8_t *bytes);
bool writeWavHeader(File &file, uint32_t dataBytes);
bool findPcmData(File &file, uint32_t &remaining);