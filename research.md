# SpecAnn Project Research

## Project Purpose
SpecAnn is a PlatformIO/Arduino sketch that turns a Metro Mini–class controller into a technician console for a three-stage local-oscillator chain in a spectrum-analyzer prototype. The firmware coordinates three MAX2871 PLLs and a PE43711 digital attenuator, accepts commands over USB serial, and exposes manual SPI controls for bring-up work. Hardware-critical operations are wrapped so automated CI can compile the project without attached peripherals.

## Build & Configuration
- `platformio.ini` defines two active environments:
  - `uno` (default target) compiles for the Arduino Uno with `SPECANN_APP` and `SPECANN_TARGET_UNO`.
  - `ci` mirrors the Uno build but defines `SPECANN_CI_BUILD`, skipping hardware-touching code and turning off tests.
- The MAX2871 driver lives in `../MAX2871_library_dev`, pulled in through `lib_deps`. That library supplies `ArduinoHAL`, `FrequencyCalculator`, and the `MAX2871` class used here.
- `src/main_entry.cpp` orchestrates hardware startup, frequency planning, and the Arduino `setup()/loop()` lifecycle, while `src/command_interface.cpp` implements the USB-serial technician console exposed via `command_interface.h`.

## Module Layout
- `main_entry.cpp` owns the MAX2871 instances, heartbeat bookkeeping, and frequency/IF helpers such as `printStatus`, `recomputePlan`, and `tuneTo`.
- `command_interface.cpp` handles serial parsing, manual SPI safety gates, and attenuator control state while exposing the minimal surface (`printBanner`, `pollSerial`, `programAttenuatorDb`, `getCurrentAttenuatorDb`, `getCurrentChipTarget`, `chipTargetName`).

## Hardware Topology & Pin Map
- Reference clock: 66.0 MHz shared by all three PLLs (`REF_MHZ`).
- Startup tuning target: 1 735.113 MHz (`STARTUP_RF_MHZ`).
- MAX2871 latch-enable pins: LO1 → A3, LO2 → D4, LO3 → A4.
- Attenuator chip-select: A5 with 1 MHz SPI transfers.
- Reference enables: `PIN_REF_EN1` = D5 (default high), `PIN_REF_EN2` = D6 (default low).
- Status LED/heartbeat: D10 toggled at 1 ms intervals (≈500 Hz).
- ArduinoHAL instances hold LE lines and drive SPI transactions; CE/MUX pins are left at defaults (not wired in this sketch).

## Global State Model
Key globals track operator-facing state:
- `currentRfInputMhz` (double) remembers the last tuned RF input.
- `desiredLo?Injection` (three `LOInjectionMode` values) persist technician overrides for high/low-side injection.
- `command_interface.cpp` keeps console-local state: `currentAttenuatorDb` (1.0–31.75 dB), `currentChipTarget`, `manualSpiArmed`, the `pendingSpi*` confirmation tuple, and the 96-byte `inputBuffer` with overflow protection.
- Heartbeat timing uses `lastHeartbeatToggleMs` and `heartbeatState`.

## Setup Sequence (`setup()`)
1. Configure the status LED, start 115200 baud serial, and wait up to 2 s for the USB CDC interface to open.
2. Seed the `FrequencyCalculator` reference clock.
3. Unless `SPECANN_CI_BUILD` is defined:
   - Call `begin()` on each `ArduinoHAL`.
   - Set pin modes for attenuator and reference enables, establishing safe default levels (attenuator idle-high, reference 1 enabled, reference 2 disabled).
   - Initialize each MAX2871 with `MAX2871::begin`, enabling both RF outputs at +5 dBm.
   - Call `programAttenuatorDb(getCurrentAttenuatorDb())` to seed the PE43711 with the default 1 dB value and emit a reminder that the coding assumes PE43711 step tables.
4. Issue the initial tuning plan via `tuneTo(STARTUP_RF_MHZ)`, print the banner, and dump system status.

## Loop Responsibilities
- `pollSerial()` (implemented in `command_interface.cpp`) consumes incoming characters, terminates on newline, sanitizes whitespace, tokenizes up to four arguments, and dispatches commands.
- `heartbeat()` flips the status LED every millisecond to provide a visual “alive” indicator.

## Command Interface
Numeric input (`23.5–6000 MHz`) tunes the synthesizers:
- Values outside the window are rejected with an error message.
- Valid entries call `tuneTo()` and then immediately display `printStatus()`.

Keyword commands:
Because the chip-select pins mix active-high and active-low devices, the console records each target’s polarity. LO1, LO2, LO3, attenuator, ref1, and ref2 assert HIGH; adc1, adc2, ram, and flash assert LOW. Ref1 and ref2 gate the system reference clocks rather than the SPI bus, so the command interface allows one (or neither) of them to remain selected across other operations while preventing both from being asserted simultaneously. For the SPI peripherals, switching targets first drives the previous chip select back to its idle level before enabling the new one so only a single device ever talks on the shared bus.
- `help` – prints the banner and command reference.
- `status` – shows the RF input, LO frequencies, IF values, injection modes, attenuator setting, and current manual target.
- `relock` – reruns initialization for each MAX2871, reapplies the last frequency, and notes completion.
- `info` – lists hardware pin assignments for technicians.
- `atten <dB>` – validates 0.25 dB steps between 1.0 and 31.75 dB, programs the PE43711, echoes the value, and refreshes status.
- `ifmode <high|low>` – applies the requested injection mode to whichever LO is currently selected via `chip`. If no LO target is active the console reports an error instead of guessing.
- `chip <lo1|lo2|lo3|atten|ref1|ref2|adc1|adc2|ram|flash>` – selects the manual SPI target using the polarity table above; new targets cover the reference enables plus ADC, RAM, and flash peripherals wired to the same bus.
- `spi <hex32>` – requires a double-entry confirmation before the first write, then forwards the 32-bit word to the active target. MAX2871 writes go through the corresponding HAL; attenuator writes mask to 7 bits; other peripherals expect the technician to supply correctly formatted payloads.

## Frequency Planning & Injection Control
- `recomputePlan()` drives the frequency calculator and hardware updates:
  - Sets `FreqRFin`, reference divider `R = 1`, and uses the calculator’s IF constants (IF1 center 3.6 GHz, IF2 315 MHz, IF3 45 MHz).
  - Aligns IF1 to the phase-detector frequency and selects LO1 injection based on technician preference (`desiredLo1Injection`).
  - LO2/LO3 frequencies come from the calculator using stored injection modes.
  - When not in CI mode, each MAX2871 receives `setFrequency()`. Afterwards the reported `freqCalc.FreqLO*` values are refreshed with `fmn2freq()` so status prints reflect actual programmed outputs.
- If LO2/LO3 computations underflow (negative frequency), warnings are printed and magnitudes are used to keep status readable.

## Attenuator Handling
- Constants define allowable range (`ATTEN_MIN_DB` = 1.0, `ATTEN_MAX_DB` = 31.75) and quantization (`ATTEN_STEP_DB` = 0.25).
- `attenCodeFromDb()` clamps and converts dB to the 7-bit code expected by the PE43711.
- `programAttenuatorRaw()` performs a 1 MHz SPI transfer with chip-select pulses and, even in CI mode, updates the cached dB value so status remains consistent.
- Status output includes a reminder about checking ≈51 Ω at maximum attenuation, matching the technician notes in `steps.md`.

## Manual SPI Safeguards
- Manual writes are locked until the same `spi` command is entered twice consecutively, preventing accidental bursts.
- Changing the chip target clears arming state.
- Every manual transaction logs `[SPI] target=<name> value=0x...` followed by a hardware damage warning.

## Safety & CI Considerations
- All HAL pin writes, SPI transactions, and attenuator programming are wrapped with `#if !defined(SPECANN_CI_BUILD)` guards. CI builds therefore execute logic paths, state transitions, and logging without touching hardware.
- Helpers such as `initializeLo()` become no-ops in CI, but data structures (e.g., `currentAttenuatorDb`) are still updated so command behavior can be tested.

## Dependencies & External Library Notes
- `ArduinoHAL` (from the MAX2871 library) abstracts SPI pin control and exposes `spiWriteRegister`, `setCEPin`, and optional ADC support. It honors a configurable SPI clock and toggles LE lines around 32-bit transfers.
- `MAX2871` maintains register shadows and exposes telemetry members (`M`, `Frac`, `N`, `DIVA`) that SpecAnn prints for diagnostics.
- `FrequencyCalculator` computes IF/LO relationships and performs the actual `MAX2871::setFrequency` calls. SpecAnn augments its default behavior by allowing technicians to flip injection modes dynamically and by re-reading programmed FMN values for reporting.

## Observations & Potential Follow-Ups
- Manual SPI currently lacks timestamping despite comments hinting at logs; adding timestamps would require minimal changes but would touch additional files.
- `Aux` chip target is a placeholder; wiring and HAL support need definition before use.
- The console warns about attenuator code assumptions but does not yet verify hardware identity; technicians should validate the PE43711 or adjust constants.
- Continuous heartbeat toggling at 1 ms may cut into loop budget if future features add heavy processing; consider making the interval configurable.
