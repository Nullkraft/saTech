# Handoff

## Current Focus

Implement the full-test technician-console flow described in `Full test.md`.

The full test runs while technician console firmware is loaded. Host-side code orchestrates the test, talks to the technician console, uses the Rigol MCP server for scope capture/decode, uses the BK390A for voltage measurement, and collates the final report.

## Design Direction

Implement and test the full-test workflow as regular Python before turning it into an Excalispur MCP server. Keep the core workflow callable from code, for example `run_full_test(config) -> report`, so the CLI and future MCP server can both use the same tested path.

`full_test.py` should be an entry point for argument parsing, configuration, report printing, and process exit status. The hardware orchestration, validation decisions, and structured report construction should live behind callable functions rather than being baked into CLI-only behavior.

## Implementation
First slice is implemented:

1. run_full_test.py
  - run_full_test(config) -> report
  - structured report data
  - no CLI parsing
2. full_test.py
  - parse args
  - build config
  - call run_full_test(config)
  - print report
  - return process status
3. First implemented workflow
  - connect to technician console
  - query id
  - verify expected response

Verified with `./bin/python full_test.py --port /dev/ttyUSB1` on actual hardware. The first run passed and returned `saTech WN2A ready`.

Next implementation step is to add focused tests around `run_full_test.py` using a fake serial object before expanding into the full technician-console sequence.

## Files To Start With

- `Full test.md`
- `src/technician_console.cpp`
- `src/main_entry.cpp`
- `src/command_interface.cpp`
- `src/command_interface.h`
- `src/frequency_calculator.cpp`
- `max2871_expected.py`
