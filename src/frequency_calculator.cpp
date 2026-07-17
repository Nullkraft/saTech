#include "frequency_calculator.h"
#include <math.h>   // round(), fabs()

// Overload A: Tech and Normal operation.
// Delegates to Overload B with both LO2 and LO3 set to High.
void FrequencyCalculator::set_LO_frequencies(double rfin, double refClockMHz, int r_div)
{
  set_LO_frequencies(rfin, refClockMHz, r_div,
                     LOInjectionMode::High,
                     LOInjectionMode::High);
}

// Overload B: Calibration operation.
// Caller explicitly specifies LO2 and LO3 injection modes.
// LO1 injection mode is computed internally from the frequency plan threshold.
void FrequencyCalculator::set_LO_frequencies(double rfin, double refClockMHz, int r_div,
                                              LOInjectionMode lo2Mode,
                                              LOInjectionMode lo3Mode)
{
  compute_LO_frequencies(rfin, refClockMHz, r_div, lo2Mode, lo3Mode);

  _lo1.setFrequency(FreqLO1);
  _lo2.setFrequency(FreqLO2);
  _lo3.setFrequency(FreqLO3);
}

void FrequencyCalculator::compute_LO_frequencies(double rfin, double refClockMHz, int r_div,
                                                  LOInjectionMode lo2Mode,
                                                  LOInjectionMode lo3Mode)
{
  FreqRFin         = rfin;
  R                = (uint8_t)r_div;
  LO2InjectionMode = lo2Mode;
  LO3InjectionMode = lo3Mode;

  // RF crossover where LO1 changes injection side based on which refClock is selected.
  // Below this threshold LO1 uses high-side injection; above it LO1 uses low-side.
  double threshold = (refClockMHz == RefClock1) ? 2343.0001 : 2403.2731;

  double fpfd     = refClockMHz / R;
  double IF1_step = fpfd * round(IF1_center / fpfd);

  // LO1 injection mode is determined by the frequency plan, not the caller.
  bool hiLo1       = (rfin < threshold);
  LO1InjectionMode = hiLo1 ? LOInjectionMode::High : LOInjectionMode::Low;
  int sign         = hiLo1 ? 1 : -1;

  FreqLO1 = fpfd * round((IF1_step + sign * rfin) / fpfd);

  IF1 = fabs(FreqLO1 - sign * rfin);   // fabs guards against rounding producing a tiny negative

  FreqLO2 = (LO2InjectionMode == LOInjectionMode::High) ? (IF1 + IF2) : (IF1 - IF2);

  FreqLO3 = (LO3InjectionMode == LOInjectionMode::High) ? (IF2 + IF3) : (IF2 - IF3);
}
