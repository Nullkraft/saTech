# Full Test Status

## Current Focus

Implement the BK390A / final amplitude slice described in `Full test.md`.

The full test runs while technician console firmware is loaded. Host-side code orchestrates the test, talks to the technician console, uses the Rigol MCP server for scope capture/decode, uses the BK390A for voltage measurement, and collates the final report.

## Design Direction

Implement and test the full-test workflow as regular Python before turning it into an Excalispur MCP server. Keep the core workflow callable from code, for example `run_full_test(config) -> report`, so the CLI and future MCP server can both use the same tested path.

`full_test.py` should be an entry point for argument parsing, configuration, report printing, and process exit status. The hardware orchestration, validation decisions, and structured report construction should live behind callable functions rather than being baked into CLI-only behavior.

## Implementation
Implemented and hardware-verified:

- `id` unit-identification check.
- `fulltest refcheck` reference enable pin report.
- `fulltest plan <MHz>` frequency-plan report.
- `fulltest program lo1` and `fulltest program lo2` planned-LO programming reports.
- `fulltest pincheck` aggregate select-pin report.
- `fulltest atten <dB>` attenuator set-point report.
- Rigol MCP capture/decode for LO1 and LO2 register verification.
- Sequential Rigol SCPI setup with pacing between writes so the scope accepts the setup commands reliably.
- Python runner report cleanup: public output now shows command list and parsed checks rather than raw serial transcripts.

Still needed for the local full-test sequence:

- Add BK390A reading, discard-settling logic, dBm conversion, and final amplitude report.

## Files To Start With

- `BRING_UP_PLAN.md`
- `Full test.md`
- `full_test.py`
- `run_full_test.py`
- `src/technician_console.cpp`
- `src/main_entry.cpp`
- `src/command_interface.cpp`
- `src/command_interface.h`
- `src/frequency_calculator.cpp`
