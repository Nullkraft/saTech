# FrequencyCalculator Injection Mode Refactor Plan

## Background

FrequencyCalculator has been moved from the MAX2871 driver library into the
SpecAnn project where it belongs. With that move, its injection mode handling
needs to be corrected to properly serve three separate programs that will share
this code: Tech, Normal, and Calibration.

---

## Injection Mode Design Decisions

| LO  | Mode Control         | Reason                                                  |
|-----|----------------------|---------------------------------------------------------|
| LO1 | Computed internally  | Determined by threshold math — deterministic from RFin  |
| LO2 | Passed by caller     | Spur mitigation — cal routine tries both sides          |
| LO3 | Passed by caller     | Spur mitigation — cal routine tries both sides          |

---

## Steps

### Step 1 — Update frequency_calculator.h

- [x] Remove the three `LOInjectionMode` member variable assignments in the
  declaration (they become outputs, not inputs, so their initial value is
  meaningless until after the first call).
- [x] Replace the single `set_LO_frequencies` declaration with two overloads:

  **Overload A** — for Tech and Normal use where LO2/LO3 default to High:
  ```cpp
  void set_LO_frequencies(double rfin, double refClockMHz, int r_div);
  ```

  **Overload B** — for Calibration use where LO2/LO3 modes are explicitly
  specified:
  ```cpp
  void set_LO_frequencies(double rfin, double refClockMHz, int r_div,
                          LOInjectionMode lo2Mode,
                          LOInjectionMode lo3Mode);
  ```

- [x] Add a comment on the `LO1InjectionMode`, `LO2InjectionMode`, and
  `LO3InjectionMode` members clarifying they are readable outputs after a call,
  not inputs to be written by the caller.

---

### Step 2 — Update frequency_calculator.cpp

- [x] Implement Overload A by calling Overload B with both modes set to High,
  keeping the logic in one place:
  ```cpp
  void FrequencyCalculator::set_LO_frequencies(double rfin, double refClockMHz,
                                                int r_div)
  {
      set_LO_frequencies(rfin, refClockMHz, r_div,
                         LOInjectionMode::High,
                         LOInjectionMode::High);
  }
  ```

- [x] Implement Overload B with the full logic:
  - [x] Store `FreqRFin`, `R`, `LO2InjectionMode`, `LO3InjectionMode` from
    parameters.
  - [x] Compute `LO1InjectionMode` internally from the threshold math (restored
    from original code — this logic belongs here, not in the caller).
  - [x] Compute `FreqLO1`, `IF1`, `FreqLO2`, `FreqLO3` as before.
  - [x] Call `setFrequency()` on each synthesizer.
  - [x] Use `fabs()` on IF1 as a rounding guard.

---

### Step 3 — Update SpecAnn call sites

- [x] Any existing call to `set_LO_frequencies` with three arguments continues to
  work unchanged via Overload A.
- [x] The Calibration program will call Overload B, explicitly passing
  `LOInjectionMode::High` or `LOInjectionMode::Low` for LO2 and LO3 on each
  step.
- [x] `recomputePlan()` in main_entry.cpp (SpecAnn) can be simplified to delegate
  entirely to Overload A, removing the duplicated frequency math currently in
  that function.

---

## What Does NOT Change

- `LO1InjectionMode` is never a parameter — it is always computed from the
  threshold math inside `set_LO_frequencies`.
- The `LOInjectionMode` enum stays in `frequency_calculator.h` — it is an
  application-level concept, not part of the MAX2871 driver.
- The MAX2871 driver files are untouched.
- `I_PLLSynthesizer.h` is untouched.
