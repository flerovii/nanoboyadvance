/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#pragma once

#include <cstdint>
#include <emulator/core/scheduler.hpp>

#include "sequencer.hpp"
#include "../registers.hpp"

namespace nba::core {

class NoiseChannel : public Sequencer {
public:
  NoiseChannel(Scheduler& scheduler, BIAS& bias);

  void Reset();
  bool IsEnabled() { return enabled; }

  void Generate(int cycles_late);
  auto Read (int offset) -> std::uint8_t;
  void Write(int offset, std::uint8_t value);

  std::int8_t sample = 0;

private:
  constexpr int GetSynthesisInterval(int ratio, int shift) {
    int interval = 64 << shift;

    if (ratio == 0) {
      interval /= 2;
    } else {
      interval *= ratio;
    }

    return interval;
  }

  std::uint16_t lfsr;

  Scheduler& scheduler;
  std::function<void(int)> event_cb = [this](int cycles_late) {
    this->Generate(cycles_late);
  };

  int  frequency_shift;
  int  frequency_ratio;
  int  width;
  bool length_enable;
  bool dac_enable;
  bool enabled;

  BIAS& bias;
  int skip_count;
};

} // namespace nba::core
