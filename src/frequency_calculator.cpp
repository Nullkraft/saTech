#include "frequency_calculator.h"
// Default plan path for standard operation: LO2 and LO3 use high-side injection.
void FrequencyCalculator::set_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div)
{
  set_LO_frequencies(rfinKHz, refClockKHz, r_div,
                     LOInjectionMode::High,
                     LOInjectionMode::High);
}

// Alternate plan path for cases that need to set LO2 and LO3 injection side.
// LO1 remains automatically selected from the RF crossover.
void FrequencyCalculator::set_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div,
                                              LOInjectionMode lo2Mode,
                                              LOInjectionMode lo3Mode)
{
  compute_LO_frequencies(rfinKHz, refClockKHz, r_div, lo2Mode, lo3Mode);

  _lo1.setFrequency(FreqLO1);
  _lo2.setFrequency(FreqLO2);
  _lo3.setFrequency(FreqLO3);
}

void FrequencyCalculator::compute_LO_frequencies(uint32_t rfinKHz, uint32_t refClockKHz, int r_div,
                                                  LOInjectionMode lo2Mode,
                                                  LOInjectionMode lo3Mode)
{
  FreqRFin         = rfinKHz;
  R                = (uint8_t)r_div;
  LO2InjectionMode = lo2Mode;
  LO3InjectionMode = lo3Mode;

  // RF crossover where LO1 changes injection side based on which refClock is selected.
  // Below this threshold LO1 uses high-side injection; above it LO1 uses low-side.
  const uint32_t threshold = (refClockKHz == RefClock1) ? LO1_REF1_THRESHOLD_KHZ : LO1_REF2_THRESHOLD_KHZ;

  const uint32_t fpfd = refClockKHz / R;
  const uint32_t IF1_step = fpfd * ((IF1_center + fpfd / 2) / fpfd);

  // LO1 injection mode is determined by the frequency plan, not the caller.
  const bool hiLo1 = (rfinKHz < threshold);
  LO1InjectionMode = hiLo1 ? LOInjectionMode::High : LOInjectionMode::Low;

  const uint32_t lo1Target = hiLo1 ? IF1_step + rfinKHz : IF1_step - rfinKHz;
  FreqLO1 = fpfd * ((lo1Target + fpfd / 2) / fpfd);

  IF1 = hiLo1
      ? (FreqLO1 >= rfinKHz ? FreqLO1 - rfinKHz : rfinKHz - FreqLO1)
      : FreqLO1 + rfinKHz;

  FreqLO2 = (LO2InjectionMode == LOInjectionMode::High) ? (IF1 + IF2) : (IF1 - IF2);

  FreqLO3 = (LO3InjectionMode == LOInjectionMode::High) ? (IF2 + IF3) : (IF2 - IF3);
}
