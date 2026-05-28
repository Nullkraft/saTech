# Handoff

## Current Focus

Implement the full-test technician-console flow described in `Full test.md`.

The full test runs while technician console firmware is loaded. Host-side code orchestrates the test, talks to the technician console, uses the Rigol MCP server for scope capture/decode, uses the BK390A for voltage measurement, and collates the final report.

## Design Direction

Implement and test the full-test workflow as regular Python before turning it into an Excalispur MCP server. Keep the core workflow callable from code, for example `run_full_test(config) -> report`, so the CLI and future MCP server can both use the same tested path.

`full_test.py` should be an entry point for argument parsing, configuration, report printing, and process exit status. The hardware orchestration, validation decisions, and structured report construction should live behind callable functions rather than being baked into CLI-only behavior.

## Files To Start With

- `Full test.md`
- `src/technician_console.cpp`
- `src/main_entry.cpp`
- `src/command_interface.cpp`
- `src/command_interface.h`
- `src/frequency_calculator.cpp`
- `max2871_expected.py`
