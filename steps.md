# SpecAnn Development Steps

## Phase 1 — Bootstrap & Infrastructure
- [x] Ensure `setup()` initializes Serial at 115200 baud with blocking wait (e.g., `while (!Serial)` for native USB boards).
- [x] Configure HAL objects for LO1/LO2/LO3 and call their `begin()` methods.
- [x] Set pin modes for attenuator select, reference enables, status LED, and any additional chip-select lines.
- [x] Apply default pin states (attenuator low, REF_EN1 high, REF_EN2 low, status LED off).
- [x] Program the initial LO plan at 1735.113 MHz via `freqCalc.set_LO_frequencies()` and log the resulting LO/IF values.
- [x] Print a startup banner with a brief command summary.

## Phase 2 — Serial Command Loop
- [x] Implement a line-buffering parser that reads commands until newline.
- [x] Support numeric frequency inputs (23.5–6000 MHz) and report errors for out-of-range values.
- [x] Implement keyword commands: `help`, `status`, `relock`, `info` (exact wording per plan).
- [x] Add heartbeat management inside `loop()` (status pin toggle) separate from command polling.

## Phase 3 — Diagnostics Helpers
- [x] Implement `printLoSummary()` that reports M, F, N values for a given MAX2871 instance.
- [x] Implement `printFrequencyPlan()` that outputs RF input, IF1, LO1/LO2/LO3.
- [x] Ensure diagnostic output is callable from both startup and the `status` command.

## Phase 4 — Attenuation Control & IF Path
- [x] Validate availability of PE43711 programming information; if missing, document deferral in code comments.
- [x] Implement `atten <dB>` command: parse 0.25 dB steps (1.0–31.75 dB), send SPI sequence to PE43711, update current state.
- [x] Implement `ifmode <lo#> <high|low>` command to flip FrequencyCalculator injection mode and reprogram LO plan.
- [x] Update status output to include current attenuator dB value and a reminder that 31.75 dB ≈ 51 Ω.

## Phase 5 — Manual SPI / Chip-Select Shell
- [x] Implement `chip <lo1|lo2|lo3|atten|aux>` command that selects which device subsequent raw SPI writes target.
- [x] Ensure the chip-selection helper always deasserts the previously active device before asserting the new one (only one CS/LE low at any time).
- [x] Implement `spi <hex32>` (and optional burst forms) to transmit raw 32-bit words to the selected device; log every transaction with timestamps.
- [x] Document onscreen that manual SPI writes can damage hardware if misused.
- [x] Add safety confirmation prompt or double-entry requirement before first risky write (optional per technician preference).

## Phase 6 — Safety & Test Hooks
- [x] Guard hardware-specific code paths with `#if !defined(SPECANN_CI_BUILD)` to enable CI builds without peripherals.
- [x] Provide no-op/mock implementations or compile-time stubs for CI mode.
- [x] Sketch initial native test plan (e.g., `pio test -e native`) to exercise command parsing logic using mocks.
      (Plan: compile with `-DSPECANN_CI_BUILD` and add future `native` environment tests using `MockHAL` to feed scripted command sequences.)

## Phase 7 — Validation & Documentation
- [x] Verify controller-only operation: run through commands with RF hardware disconnected while monitoring SPI lines on the scope.
      - Procedure: power the controller and reference supplies only, connect a scope to SCK/MOSI/LE lines, issue `status`, a few frequency tunes (`2400`, `5800`), `atten 10.0`, and confirm SPI bursts appear without RF hardware attached.
- [x] Document expected scope traces for the status pin, PE43711 latch, and MAX2871 LE lines.
      - Status pin D10: 500 Hz square wave (1 ms high / 1 ms low). PE43711 latch (A5): 1 MHz burst ~8 µs wide per atten command; MAX2871 LE pins pulse high for ~1 µs on each register write.
- [x] Prepare brief technician usage notes (command reference, caution statements, troubleshooting tips).
      - Notes: begin with `help`; confirm attenuator writes with `status`; use `chip`/`spi` only after double-entry arming; lock warnings appear via `relock`; reminder that 31.75 dB ≈ 51 Ω for DVM checks.
