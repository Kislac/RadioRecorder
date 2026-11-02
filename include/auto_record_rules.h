#pragma once

#include <array>
#include <cstdint>

enum class RecordingMode : uint8_t { manualMic, manualRadio, autoRadio, autoMic, none = 255 };

struct AutoRecordConfig {
  uint16_t threshold;
  uint8_t silenceSeconds;
  constexpr AutoRecordConfig(uint16_t level = 100, uint8_t silence = 3) : threshold(level), silenceSeconds(silence) {}
};

struct AutoDetector {
  bool armed = false;
  bool hasSample = false;
  uint16_t rms = 0;
  uint8_t aboveCount = 0;
  uint64_t armedAt = 0, sampledAt = 0, lastSoundAt = 0;

  void arm(bool enabled, uint64_t now) {
    armed = enabled; hasSample = false; aboveCount = 0;
    armedAt = sampledAt = lastSoundAt = now;
  }

  void observe(uint16_t value, uint64_t now, const AutoRecordConfig &config, bool recording) {
    rms = value; sampledAt = now; hasSample = true;
    if (!armed || now - armedAt < 2000) { aboveCount = 0; lastSoundAt = now; return; }
    uint16_t level = recording ? static_cast<uint16_t>((static_cast<uint32_t>(config.threshold) * 80 + 99) / 100) : config.threshold;
    if (value >= level) {
      if (aboveCount < 2) ++aboveCount;
      lastSoundAt = now;
    } else aboveCount = 0;
  }

  bool ready(uint64_t now) const { return armed && hasSample && now - sampledAt <= 750 && now - armedAt >= 2000 && aboveCount >= 2; }
  bool silent(uint64_t now, const AutoRecordConfig &config) const {
    return !armed || (hasSample && now - lastSoundAt >= static_cast<uint64_t>(config.silenceSeconds) * 1000);
  }
};

inline bool validAutoConfig(const AutoRecordConfig &config, bool microphone) {
  return config.threshold >= 1 && config.threshold <= (microphone ? 32768 : 4095) &&
         config.silenceSeconds >= 1 && config.silenceSeconds <= 60;
}

inline bool validRecordingPriorities(const std::array<RecordingMode, 4> &priorities) {
  uint8_t mask = 0;
  for (auto mode : priorities) {
    uint8_t value = static_cast<uint8_t>(mode);
    if (value > 3 || (mask & (1 << value))) return false;
    mask |= 1 << value;
  }
  return mask == 15;
}

inline RecordingMode chooseRecordingMode(const std::array<RecordingMode, 4> &priorities,
                                         const std::array<bool, 4> &requested) {
  for (auto mode : priorities) if (requested[static_cast<uint8_t>(mode)]) return mode;
  return RecordingMode::none;
}