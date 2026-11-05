#pragma once

#include <stdint.h>
#include "I_PLLSynthesizer.h"

enum class LOInjectionMode : uint8_t { Low, High };

class FrequencyCalculator {
public:
  uint8_t R = 1;
  uint32_t IF1 = 0;
  uint32_t IF1_center = 3600000;
  uint32_t IF2 = 315000;
  uint32_t IF3 = 45000;
  uint32_t RefClock1 = 66000;
  uint32_t RefClock2 = 66666;
  static constexpr uint32_t LO1_REF1_THRESHOLD_KHZ = 2343000;
  static constexpr uint32_t LO1_REF2_THRESHOLD_KHZ = 2403273;

  // Read-only outputs — injection modes selected by the most recent frequency plan.
  LOInjectionMode LO1InjectionMode;  // LO1 injection side chosen automatically from the plan crossover.
  LOInjectionMode LO2InjectionMode;  // LO2 injection side used in the computed plan.
  LOInjectionMode LO3InjectionMode;  // LO3 injection side used in the computed plan.

  uint32_t FreqRFin = 0;
  uint32_t FreqLO1  = 0;
  uint32_t FreqLO2  = 0;
  uint32_t FreqLO3  = 0;

  FrequencyCalculator(I_PLLSynthesizer& lo1, I_PLLSynthesizer& lo2, I_PLLSynthesizer& lo3)
    : LO1InjectionMode(LOInjectionMode::High),
      LO2InjectionMode(LOInjectionMode::High),
      LO3InjectionMode(LOInjectionMode::High),
      _lo1(lo1), _lo2(lo2), _lo3(lo3) {}

  // Default planning entry point for normal operation.
  // LO2 and LO3 use the standard high-side plan; LO1 is chosen by the frequency plan.
  void set_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div);

  // Planning entry point when LO2 and LO3 injection sides must be forced explicitly,
  // such as calibration or spur-mitigation work. LO1 remains plan-selected.
  void set_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div,
                          LOInjectionMode lo2Mode,
                          LOInjectionMode lo3Mode);

  void compute_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div,
                              LOInjectionMode lo2Mode,
                              LOInjectionMode lo3Mode);

private:
  I_PLLSynthesizer& _lo1;
  I_PLLSynthesizer& _lo2;
  I_PLLSynthesizer& _lo3;
};
