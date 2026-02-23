# SpecAnn.cpp Development Plan

## Goals
- Accept a single RF input frequency over the USB serial port and configure the three MAX2871 PLLs accordingly.
- Leverage the existing MAX2871 library (`ArduinoHAL`, `MAX2871`, `FrequencyCalculator`) without modifying its internals.
- Provide responsive feedback to the user (acknowledgements, error messages, status dumps).
- Keep the sketch testable without RF hardware attached by guarding hardware-dependent paths.
- Pin selects and SPI writes will be tested with the o'scope

## System Overview
1. **Hardware layer:** Three `ArduinoHAL` instances, one per LO (LE pins on Metro Mini: A3, 4, A4).
2. **Synthesizers:** `MAX2871 lo1/lo2/lo3` configured with 66 MHz reference clock.
3. **Coordinator:** `FrequencyCalculator` computes LO frequencies for a requested RF input.
4. **UI layer:** Serial command loop that parses numeric MHz inputs and optional commands (`help`, `status`, `relock`, etc.).

```
Serial (host) ──> Command Parser ──> FrequencyCalculator#set_LO_frequencies ──┐
                                                                              ├──> MAX2871::setFrequency / output control
User feedback <── Status Reporter <───────────────────────────────────────────┘
```

## Work Breakdown
1. **Bootstrap (`setup`)**
   - Initialize serial at 115200 baud with blocking wait.
   - Call `begin()` on each HAL and PLL, configure outputs (`outputSelect`, `outputPower`), set reference enables.
   - Program an initial LO plan with `freqCalc.set_LO_frequencies(1735.113, freqCalc.RefClock1, 1);`.
   - Print banner and usage instructions.

2. **Serial Command Loop (`loop`)**
   - Poll `Serial.available()`; buffer characters until end-of-line.
   - Parse commands:
     - **Numeric** (e.g., `2412.5`): validate range (23.5–6000 MHz), call `freqCalc.set_LO_frequencies()`, then dump resulting LO/IF values.
     - **Keywords** (`help`, `status`, `relock`, `info`): map to helper functions.
   - Gracefully handle invalid input (timeout, non-numeric).

3. **Diagnostics Helpers**
   - `void printLoSummary(const char* name, const MAX2871& lo);`
   - `void printFrequencyPlan(const FrequencyCalculator& fc);`

4. **Attenuation Control & IF Path**
   - Drive the PE43711 digital attenuator chip-select/LATCH during `setup()` and verify on the scope; it supports 0.25 dB steps from 1.0 dB to 31.75 dB, so we need to confirm we have (or obtain) its SPI programming table—otherwise defer this feature.
   - Extend the command parser with `atten <dB>` to program the PE43711 and `ifmode <lo#> <high|low>` to select high- vs. low-side injection for each LO.
   - Include the current attenuator setting in status output and remind testers that 31.75 dB should measure ~51 Ω on a DVM for sanity checks.

5. **Safety / Hardware Guards**
   - Wrap hardware-touching code (`SpecAnn::begin`, attenuator writes, lock-checks) with `#if !defined(SPECANN_CI_BUILD)` so CI builds run logic without requiring peripherals.
   - Keep a simulation-friendly path (mock HAL / no-op handlers) for future native tests.

6. **Future Hooks**
   - Optional `bool checkLock(MAX2871& lo);` that queries `isLocked()` and reports (tolerant of stub returns).
   - Add EEPROM persistence for last-tuned frequency.

## File Layout
```
src/
├─ SpecAnn.cpp      ← main sketch entry (single translation unit)
└─ main_entry.cpp   ← thin wrapper calling into SpecAnn.cpp (optional)
```

## Implementation Sketch

### Entry Point
```cpp
#include "SpecAnn.h"

void setup() {
    SpecAnn::instance().begin();
}

void loop() {
    SpecAnn::instance().poll();
}
```

### SpecAnn Singleton Outline
```cpp
class SpecAnn {
public:
    static SpecAnn& instance();

    void begin();
    void poll();

private:
    SpecAnn();

    void handleCommand(const String& cmd);
    void tune(double mhz);
    void printStatus() const;

    ArduinoHAL halLo1, halLo2, halLo3;
    MAX2871 lo1, lo2, lo3;
    FrequencyCalculator freqCalc;
};
```

### Command Handling Example
```cpp
void SpecAnn::handleCommand(const String& cmd) {
    if (cmd.equalsIgnoreCase("help")) {
        Serial.println(F("Enter frequency MHz (23.5–6000) or commands: help, status, relock"));
        return;
    }
    char* end = nullptr;
    double mhz = cmd.toFloat();
    if (mhz >= 23.5 && mhz <= 6000.0) {
        tune(mhz);
    } else {
        Serial.println(F("Invalid entry. Try e.g. 2412.5"));
    }
}
```

### Tuning Routine Snippet
```cpp
void SpecAnn::tune(double mhz) {
    freqCalc.set_LO_frequencies(mhz, freqCalc.RefClock1, 1);
    printStatus();

#if !defined(SPECANN_CI_BUILD)
    halLo1.setCEPin(true);
    halLo2.setCEPin(true);
    halLo3.setCEPin(true);
#endif
}
```

### Status Reporting
```cpp
void SpecAnn::printStatus() const {
    Serial.println(F("\nFrequency Plan"));
    Serial.print(F("RF In: ")); Serial.print(freqCalc.FreqRFin, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO1 : ")); Serial.print(freqCalc.FreqLO1, 3);
    Serial.print(F("  IF1: ")); Serial.println(freqCalc.IF1, 3);
    Serial.print(F("LO2 : ")); Serial.print(freqCalc.FreqLO2, 3);
    Serial.print(F("LO3 : ")); Serial.println(freqCalc.FreqLO3, 3);

    auto showLo = [&](const char* name, const MAX2871& lo) {
        Serial.print(name);
        Serial.print(F(" M=")); Serial.print(lo.M);
        Serial.print(F(" F=")); Serial.print(lo.Frac);
        Serial.print(F(" N=")); Serial.println(lo.N);
    };
    showLo("LO1", lo1);
    showLo("LO2", lo2);
    showLo("LO3", lo3);
}
```

## Validation Strategy
- **Host-only:** Define `SPECANN_CI_BUILD` and run `pio test` (native) with mocks to exercise parsing.
- **Controller-only:** With RF hardware disconnected, send frequencies over serial; observe SPI traffic and status prints.
- **Full hardware:** Connect PLL boards, monitor LO outputs with spectrum analyzer; extend tests to check `isLocked()` and power levels.

## Milestones
1. **MVP (Controller Only)** – Serial parsing + frequency calculator with dummy hardware (current focus).
2. **Hardware Bring-up** – Validate tuning on real MAX2871 boards; add lock reporting.
3. **Analyzer Integration** – Combine with ADC capture, attenuation control, and host tooling.
