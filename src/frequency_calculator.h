#pragma once

#include <stdint.h>
#include "I_PLLSynthesizer.h"

enum class LOInjectionMode : uint8_t { Low, High };

class FrequencyCalculator {
public:
  uint8_t R = 1;
  double IF1 = 0.0;
  double IF1_center = 3600.0;
  double IF2 = 315.0;
  double IF3 = 45.0;
  double RefClock1 = 66.000;
  double RefClock2 = 66.666;
  static constexpr double LO1_REF1_THRESHOLD_MHZ = 2343.0001;
  static constexpr double LO1_REF2_THRESHOLD_MHZ = 2403.2731;

  // Read-only outputs — reflect the modes used in the last computed plan.
  // Do not write these directly.
  LOInjectionMode LO1InjectionMode;  // Computed internally from threshold math
  LOInjectionMode LO2InjectionMode;  // Reflects lo2Mode passed by caller
  LOInjectionMode LO3InjectionMode;  // Reflects lo3Mode passed by caller

  double FreqRFin = 0.0;
  double FreqLO1  = 0.0;
  double FreqLO2  = 0.0;
  double FreqLO3  = 0.0;

  FrequencyCalculator(I_PLLSynthesizer& lo1, I_PLLSynthesizer& lo2, I_PLLSynthesizer& lo3)
    : LO1InjectionMode(LOInjectionMode::High),
      LO2InjectionMode(LOInjectionMode::High),
      LO3InjectionMode(LOInjectionMode::High),
      _lo1(lo1), _lo2(lo2), _lo3(lo3) {}

  // Overload A: Tech and Normal operation.
  // LO2 and LO3 injection modes default to High.
  // LO1 injection mode is computed internally from the frequency plan threshold.
  void set_LO_frequencies(double rfin, double refClockMHz, int r_div);

  // Overload B: Calibration operation.
  // Caller explicitly specifies LO2 and LO3 injection modes for spur mitigation.
  // LO1 injection mode is still computed internally from the frequency plan threshold.
  void set_LO_frequencies(double rfin, double refClockMHz, int r_div,
                          LOInjectionMode lo2Mode,
                          LOInjectionMode lo3Mode);

  void compute_LO_frequencies(double rfin, double refClockMHz, int r_div,
                              LOInjectionMode lo2Mode,
                              LOInjectionMode lo3Mode);

private:
  I_PLLSynthesizer& _lo1;
  I_PLLSynthesizer& _lo2;
  I_PLLSynthesizer& _lo3;
};
