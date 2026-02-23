# SpecAnn Technician Console Plan

## Goals
- Accept numeric RF frequency requests over USB serial and drive all three MAX2871 PLLs accordingly.
- Expose a technician-friendly console that also allows attenuator control, LO injection selection, explicit LO programming (`lofreq`), and manual SPI access.
- Keep technician-visible state easy to inspect in code (`console_state`) so bench troubleshooting lines up with runtime output.
- Leverage the MAX2871 support library (`ArduinoHAL`, `MAX2871`, `FrequencyCalculator`) without modifying its internals.
- Maintain CI safety by compiling with `SPECANN_CI_BUILD` to skip hardware writes while preserving state transitions.
- Verify pin selects and SPI transactions on the scope, with special treatment for the ref clock selects (ref1/ref2) remaining mutually exclusive even when they stay asserted.

## System Overview
1. **Hardware layer (`main_entry.cpp`):** Owns the three `ArduinoHAL` instances (Metro Mini LE pins A3, D4, A4), the MAX2871 objects with a 66 MHz reference, the frequency calculator, and heartbeat/status GPIO.
2. **Technician console (`command_interface.cpp`):** Parses serial input, manages command state (attenuator dB, chip target, manual SPI arming), and calls back into hardware helpers such as `tuneTo()` and `recomputePlan()`.
3. **Shared console state (`console_state.h/.cpp`):** Centralizes technician-facing variables (attenuator value, selected device, confirmation flags) so the source code exposes the same state technicians see on the console.
4. **Command surface:** Numeric MHz entries and keywords (`help`, `status`, `relock`, `info`, `atten`, `ifmode`, `lofreq`, `chip`, `spi`) with polarity-aware chip select handling—LO/atten/ref pins assert HIGH, ADC/RAM/flash assert LOW, and ref1/ref2 remain mutually exclusive while allowed to stay asserted.

```
Serial host ──> command_interface.cpp (parse/tokens) ──┐
                                                      ├──> console_state (chip, atten, SPI guard)
                                                      ├──> tuneTo()/recomputePlan()/printStatus()
                                                      └──> manual SPI writer / attenuator driver
```

## Work Breakdown
1. **Bootstrap (`setup`)**
   - Initialize serial at 115200 baud with a short wait for USB.
   - Initialize HALs and MAX2871 devices (`begin`, `outputSelect`, `outputPower`), set attenuator/ref enable pins, seed the `FrequencyCalculator` reference, and call `programAttenuatorDb(getCurrentAttenuatorDb())`.
   - Call `tuneTo(STARTUP_RF_MHZ)` → `recomputePlan()` to program the synths, print console banner/status, and start the heartbeat timer.

2. **Serial Command Loop (`loop` + `pollSerial`)**
   - Buffer up to 96 chars, trim whitespace, tokenize (max 4 tokens), and dispatch:
     - **Numeric MHz**: range-check (23.5–6000), call `tuneTo`, then `printStatus`.
     - **Keywords**:
       - `help`, `status`, `relock`, `info`
       - `atten <dB>`: enforce 1.0–31.75 dB in 0.25 increments, send PE43711 code, update console state, print reminder.
       - `ifmode <high|low>`: apply injection mode to selected LO; reject if no LO target selected.
       - `lofreq <MHz>`: program the selected LO via the calculator/`setFrequency()` path; reject without an active LO.
       - `chip <lo1|lo2|lo3|atten|ref1|ref2|adc1|adc2|ram|flash>`: switch targets respecting HIGH/LOW polarity; keep ref1/ref2 mutually exclusive while allowing one to stay asserted.
       - `spi <hex32>`: enforce double-entry confirmation before arming manual writes, then forward to the active device.
   - On overflow, reset the buffer and warn; ignore carriage returns.

3. **Diagnostics Helpers**
   - `void printStatus()` → prints frequency plan, LO summaries, injection modes, attenuator dB, active chip.
   - `static void printFrequencyPlan()` / `static void printLoSummary(const __FlashStringHelper*, const MAX2871&)` output the detailed plan from `freqCalc`.

4. **Attenuation & Manual SPI**
   - `command_interface.cpp` drives attenuator programming (`programAttenuatorDb`, `programAttenuatorRaw`), storing the dB value in `ConsoleState` for printouts/tests.
   - Manual SPI flow uses `ConsoleState` flags (`manualSpiArmed`, `pendingSpi*`) to gate writes, logs every transaction, and supports LO, attenuator, ref clock, ADC, RAM, flash targets.

5. **Safety / Hardware Guards**
   - Wrap pin toggles, SPI writes, and MAX2871 programming with `#if !defined(SPECANN_CI_BUILD)` so CI runs logic without hardware effects while still updating state.
   - Provide `resetConsoleState()` for future test scaffolding.

6. **Future Hooks**
   - Optional lock-check helper (`checkLock`) and timestamped manual SPI logging.
   - Store last-tuned frequency/attenuator in EEPROM for power-cycle persistence.

## File Layout
```
src/
└─ main_entry.cpp   ← primary sketch file (setup/loop plus helper functions)
```

## Implementation Sketch

### Entry Point & Helpers
```cpp
// Globals map 1:1 to hardware so technicians can tweak them directly.
static ArduinoHAL halLo1(PIN_LE_LO1);
static MAX2871   lo1(REF_MHZ, halLo1);
static FrequencyCalculator freqCalc(lo1, lo2, lo3);

void setup() {
    Serial.begin(115200);
    pinMode(PIN_ATTEN, OUTPUT);
    halLo1.begin();
    initializeLo(lo1);
    resetConsoleState();     // set attenuator/chip defaults
    programAttenuatorDb(getCurrentAttenuatorDb());
    tuneTo(startupMHz);
    printBanner();
    printStatus();
}

void loop() {
    pollSerial();            // parse technician commands
    heartbeat();             // toggle visible status pin
}
```

### Command Handling Example
```cpp
static void handleCommand(const String& cmd) {
    if (cmd.equalsIgnoreCase("help")) {
        Serial.println(F("Enter frequency MHz (23.5–6000) or commands: help, status, atten <dB>, ifmode <high|low>, lofreq <MHz>, chip <...>, spi <hex32>"));
        return;
    }
    double mhz = cmd.toFloat();
    if (mhz >= 23.5 && mhz <= 6000.0) {
        tuneTo(mhz);
        printStatus();
        return;
    }
    Serial.println(F("Invalid entry. Try e.g. 2412.5 or 'help'."));
}
```

### Status Reporting
```cpp
static void printFrequencyPlan() {
    Serial.println(F("\nFrequency Plan"));
    Serial.print(F("RF In : ")); Serial.print(freqCalc.FreqRFin, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO1  : ")); Serial.print(freqCalc.FreqLO1, 3);
    Serial.print(F(" MHz  IF1: ")); Serial.println(freqCalc.IF1, 3);
    Serial.print(F("LO2  : ")); Serial.print(freqCalc.FreqLO2, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO3  : ")); Serial.print(freqCalc.FreqLO3, 3); Serial.println(F(" MHz"));
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
