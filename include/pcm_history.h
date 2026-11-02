#pragma once

#include <cstddef>
#include <cstdint>

template<size_t Capacity>
struct PcmHistory {
  uint8_t data[Capacity] = {};
  size_t next = 0, count = 0;
  void clear() { next = count = 0; }
  void push(const uint8_t *samples, size_t length) {
    for (size_t index = 0; index < length; ++index) {
      data[next] = samples[index]; next = (next + 1) % Capacity;
      if (count < Capacity) ++count;
    }
  }
  template<class Output>
  bool write(Output &output, uint32_t &written) const {
    size_t start = (next + Capacity - count) % Capacity;
    size_t first = count < Capacity - start ? count : Capacity - start;
    if (first && output.write(data + start, first) != first) return false;
    size_t second = count - first;
    if (second && output.write(data, second) != second) return false;
    written += count; return true;
  }
};