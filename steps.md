# SpecAnn Development Steps

## Phase 1 — Bootstrap & Infrastructure
- [ ] Ensure `setup()` initializes Serial at 115200 baud with blocking wait (e.g., `while (!Serial)` for native USB boards).
- [ ] Configure HAL objects for LO1/LO2/LO3 and call their `begin()` methods.
- [ ] Set pin modes for attenuator select, reference enables, status LED, and any additional chip-select lines.
- [ ] Apply default pin states (attenuator low, REF_EN1 high, REF_EN2 low, status LED off).
- [ ] Program the initial LO plan at 1735.113 MHz via `freqCalc.set_LO_frequencies()` and log the resulting LO/IF values.
- [ ] Print a startup banner with a brief command summary.

## Phase 2 — Serial Command Loop
- [ ] Implement a line-buffering parser that reads commands until newline.
- [ ] Support numeric frequency inputs (23.5–6000 MHz) and report errors for out-of-range values.
- [ ] Implement keyword commands: `help`, `status`, `relock`, `info` (exact wording per plan).
- [ ] Add heartbeat management inside `loop()` (status pin toggle) separate from command polling.

## Phase 3 — Diagnostics Helpers
- [ ] Implement `printLoSummary()` that reports M, F, N values for a given MAX2871 instance.
- [ ] Implement `printFrequencyPlan()` that outputs RF input, IF1, LO1/LO2/LO3.
- [ ] Ensure diagnostic output is callable from both startup and the `status` command.

## Phase 4 — Attenuation Control & IF Path
- [ ] Validate availability of PE43711 programming information; if missing, document deferral in code comments.
- [ ] Implement `atten <dB>` command: parse 0.25 dB steps (1.0–31.75 dB), send SPI sequence to PE43711, update current state.
- [ ] Implement `ifmode <lo#> <high|low>` command to flip FrequencyCalculator injection mode and reprogram LO plan.
- [ ] Update status output to include current attenuator dB value and a reminder that 31.75 dB ≈ 51 Ω.

## Phase 5 — Manual SPI / Chip-Select Shell
- [ ] Implement `chip <lo1|lo2|lo3|atten|aux>` command that selects which device subsequent raw SPI writes target.
- [ ] Ensure the chip-selection helper always deasserts the previously active device before asserting the new one (only one CS/LE low at any time).
- [ ] Implement `spi <hex32>` (and optional burst forms) to transmit raw 32-bit words to the selected device; log every transaction with timestamps.
- [ ] Document onscreen that manual SPI writes can damage hardware if misused.
- [ ] Add safety confirmation prompt or double-entry requirement before first risky write (optional per technician preference).

## Phase 6 — Safety & Test Hooks
- [ ] Guard hardware-specific code paths with `#if !defined(SPECANN_CI_BUILD)` to enable CI builds without peripherals.
- [ ] Provide no-op/mock implementations or compile-time stubs for CI mode.
- [ ] Sketch initial native test plan (e.g., `pio test -e native`) to exercise command parsing logic using mocks.

## Phase 7 — Validation & Documentation
- [ ] Verify controller-only operation: run through commands with RF hardware disconnected while monitoring SPI lines on the scope.
- [ ] Document expected scope traces for the status pin, PE43711 latch, and MAX2871 LE lines.
- [ ] Prepare brief technician usage notes (command reference, caution statements, troubleshooting tips).
