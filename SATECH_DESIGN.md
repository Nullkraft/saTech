# saTech Deterministic Verification Design

## Problem

The current `Verify <MHz>` workflow is functionally valid, but its orchestration is not deterministic enough. The Rigol and BK390A paths already run through MCP tools with device discovery and structured responses. The `saTech` path still depends on an ad hoc shell-held serial session, which makes the routine sensitive to stdin lifetime, PTY behavior, sandbox device visibility, command pacing, and output-drain heuristics.

Observed failure modes:

- A foreground Python serial bridge exited when stdin reached EOF.
- A PTY-held helper closed immediately when the driving process ended.
- A background daemon in `/tmp` did not survive the sandboxed shell lifetime.
- `/dev` visibility differed between sandboxed shell commands and MCP hardware tools.
- A Python helper launched with `./bin/python - <<'PY'` consumed stdin for the script body, leaving no live stdin for later `id` or `fulltest` commands.
- Back-to-back SA commands allowed `RFin 123.345` to return a structured plan response while the immediately queued `fulltest plan 20` returned empty.
- Expected SPI writes had to be derived manually from the staged plan fields after the SA response was captured.

The key issue is that `saTech` is not represented as a persistent, structured hardware endpoint in the same way the Rigol scope is.

## Design Goal

Make `Verify <MHz>` a deterministic, structured hardware routine:

- The requested `target_mhz` is validated over the full `0.0` to `3000.0` MHz RFin range.
- The `saTech` UART is opened once and owned by a long-lived process.
- Commands are serialized through one lock.
- Each command has a complete response before the next command is sent.
- SA report output is parsed into typed values and checks.
- Rigol setup, trigger state, SPI decode, BK390A read, and AD8307 conversion are coordinated by one high-level verification routine.
- The final result is one structured report with explicit pass/fail status for every stage.

## saTech Session Server

Add a small `saTech` MCP server or equivalent long-lived host process with one persistent serial object.

Suggested tools:

```text
satech_list_ports()
satech_open_session(port=None, baud=115200)
satech_close_session()
satech_session_status()
satech_command(command, expect_report=None, timeout_s=8.0)
satech_id()
satech_stage_rfin(target_mhz)
satech_fulltest_plan(target_mhz)
satech_fulltest_program(lo)
```

Session behavior:

- Resolve the CP2102N USB-to-UART device when no port is provided.
- Open the technician console at `115200` baud.
- Wait `3.0` seconds after opening.
- Clear the startup buffer.
- Drain serial input until quiet.
- Send `id`.
- Require `saTech WN2A ready`.
- Keep the serial handle open until `satech_close_session()`.
- Use one process-local lock around all serial write/read cycles.

Internal shape:

```python
class SaTechSession:
    def __init__(self):
        self.port = None
        self.baud = 115200
        self.ser = None
        self.lock = threading.Lock()

    def open(self, port=None, baud=115200):
        ...

    def close(self):
        ...

    def command(self, text, expect_report=None, timeout_s=8.0):
        ...

    def read_report(self, expect_report=None, timeout_s=8.0):
        ...
```

## SA Response Model

The session server should normalize SA output immediately. Callers should not parse raw terminal text.

Example normalized `fulltest plan 20.000` response:

```json
{
  "status": "ok",
  "command": "fulltest plan 20.000",
  "report": "plan",
  "values": {
    "rfin_mhz": 20.0,
    "if1_mhz": 3610.0,
    "if2_mhz": 315.0,
    "lo1_frequency_mhz": 3630.0,
    "lo1_m": 4095,
    "lo1_f": 0,
    "lo1_n": 55,
    "lo1_diva": 1,
    "lo1_injection": "High",
    "lo2_frequency_mhz": 3925.0,
    "lo2_m": 2293,
    "lo2_f": 1077,
    "lo2_n": 59,
    "lo2_diva": 1,
    "lo2_injection": "High"
  },
  "checks": [
    {
      "name": "lo1_frequency_mhz",
      "expected": 3630.0,
      "actual": 3630.0,
      "result": "PASS"
    }
  ],
  "raw_lines": []
}
```

Parsing rules:

- Parse JSON report lines from firmware as structured objects.
- Group lines between `report_begin` and `report_end`.
- Store `value` lines in a `values` map by name.
- Store `check` lines in a `checks` list.
- Return raw lines for diagnostics, but keep structured values as the primary API.
- Treat missing expected reports, malformed JSON, timeout, or unexpected EOF as explicit errors.

Firmware improvement if available:

- Add an unambiguous command completion marker or prompt.
- Include a command echo or request id in every response.
- Then the host can wait for exact completion instead of relying on quiet-time draining.

## Expected MAX2871 Dirty Writes

Expected SPI writes must come from firmware-staged plan fields, not from previous-frequency-to-target-frequency math.

Dirty register helpers:

```text
R0 = (N << 15) | (F << 3) | 0
R1 = 0x40010000 | (M << 3) | 1
```

Expected write rules:

- LO1 commonly emits `R0` when only `N` or `F` changes.
- LO1 emits `R4` as well when `DIVA` changes.
- LO2 commonly emits `R1` and `R0` when `M`, `F`, or `N` changes.
- The coordinator should compare decoded writes by ordered address and full 32-bit value.
- If future firmware changes alter dirty-register behavior, update the expected-write builder rather than the capture routine.

## High-Level Coordinator Tool

Add one high-level routine:

```text
verify_frequency(target_mhz, sa_port=None, bk_port=None)
```

Responsibilities:

1. Open or reuse a validated `saTech` session.
2. Resolve the Rigol once with `list_ports()`.
3. Run `rigol_ds1102e_setup_for_spi_bus_analysis(verify=True)`.
4. Require the scope setup verification to pass.
5. Send `RFin 123.345`.
6. Send `fulltest plan <target_mhz>`.
7. Build expected LO1 and LO2 dirty writes from staged plan fields.
8. Arm the scope for LO1 with `:RUN`.
9. Confirm Rigol trigger status is `WAIT`.
10. Send `fulltest program lo1`.
11. Confirm Rigol trigger status is `STOP`.
12. Decode LO1 SPI with expected addresses.
13. Compare LO1 decoded writes with expected writes.
14. Arm the scope for LO2 with `:RUN`.
15. Confirm Rigol trigger status is `WAIT`.
16. Send `fulltest program lo2`.
17. Confirm Rigol trigger status is `STOP`.
18. Decode LO2 SPI with expected addresses.
19. Compare LO2 decoded writes with expected writes.
20. Resolve the BK390A once with `list_ports()`.
21. Read the BK390A with `require_stable=True`, `max_frames=6`, and `timeout_s=2.0`.
22. Require a stable DC voltage reading.
23. Convert AD8307 voltage to dBm with `Pin_dBm = (Vout_mV / 25) - 84`.
24. Close the held `saTech` session in a `finally` block unless the caller explicitly requested reuse.
25. Return one structured verification report.

Hard scope setup pass criteria:

- CH1 display is on.
- CH2 display is on.
- CH1 scale is `2.0`.
- Trigger mode is edge.
- Trigger source is CH1.
- Trigger level is `1.28`.
- Trigger sweep is single.
- Waveform points mode is `RAW`.
- Timebase scale is `5.0us`.

Example result:

```json
{
  "status": "ok",
  "overall_result": "PASS",
  "target_mhz": 20.0,
  "sa": {
    "port": "/dev/ttyUSB1",
    "id": "saTech WN2A ready"
  },
  "rigol": {
    "device": "/dev/usbtmc0",
    "setup_result": "PASS"
  },
  "lo1": {
    "expected": [
      {"address": 0, "hex": "0x001B8000"}
    ],
    "decoded": [
      {"address": 0, "hex": "0x001B8000"}
    ],
    "result": "PASS"
  },
  "lo2": {
    "expected": [
      {"address": 1, "hex": "0x400147A9"},
      {"address": 0, "hex": "0x001DA1A8"}
    ],
    "decoded": [
      {"address": 1, "hex": "0x400147A9"},
      {"address": 0, "hex": "0x001DA1A8"}
    ],
    "result": "PASS"
  },
  "bk390a": {
    "port": "/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller_D-if00-port0",
    "raw_frame": "10484;00:",
    "voltage_v": 0.484,
    "pin_dbm": -64.64
  }
}
```

## Determinism Invariants

The coordinator should enforce these invariants:

- Only one active `saTech` session owns the UART.
- Only one SA command can be in flight at a time.
- No SA command is sent until the previous command has returned a complete response or a hard error.
- Scope setup verification must pass before any SA programming capture.
- Scope status must be `WAIT` before sending `fulltest program lo1` or `fulltest program lo2`.
- Scope status must be `STOP` before SPI decode.
- SPI decode must use expected write counts and expected register addresses.
- Expected dirty writes are derived from staged plan fields.
- BK390A discovery must use the returned `resolved_default_port`.
- BK390A measurement must be stable and must decode as DC voltage.
- AD8307 conversion must use `Vout_mV = measurement.value * 1000` and `Pin_dBm = (Vout_mV / 25) - 84`.
- Overall `PASS` requires scope setup, LO1 SPI, LO2 SPI, and BK390A conversion to complete successfully.

## Required Report Fields

The final verification report should include:

- Requested `target_mhz`.
- Resolved Rigol device path.
- Resolved BK390A port.
- SA console port.
- Scope setup verification result.
- `RFin 123.345` response.
- `fulltest plan <target_mhz>` response.
- LO1 expected dirty registers.
- LO1 decoded dirty registers.
- LO1 pass/fail result.
- LO2 expected dirty registers.
- LO2 decoded dirty registers.
- LO2 pass/fail result.
- BK390A raw frame and decoded voltage.
- AD8307 dBm conversion.
- Overall pass/fail result.

## Known-Good Register Vectors

These live vectors came from successful hardware runs and should become focused tests for the expected-write builder and coordinator comparisons.

`Verify 10.000`:

- Staged LO1 fields: frequency `3630.000 MHz`, `M=4095`, `F=0`, `N=55`, `DIVA=1`.
- Staged LO2 fields: frequency `3935.000 MHz`, `M=2603`, `F=1617`, `N=59`, `DIVA=1`.
- Expected LO1 dirty writes: `R0 0x001B8000`.
- Expected LO2 dirty writes: `R1 0x40015159`, `R0 0x001DB288`.

`Verify 20.000`:

- Staged LO1 fields: frequency `3630.000 MHz`, `M=4095`, `F=0`, `N=55`, `DIVA=1`.
- Staged LO2 fields: frequency `3925.000 MHz`, `M=2293`, `F=1077`, `N=59`, `DIVA=1`.
- Expected LO1 dirty writes: `R0 0x001B8000`.
- Expected LO2 dirty writes: `R1 0x400147A9`, `R0 0x001DA1A8`.

`Verify 2365.913`:

- Staged LO1 fields: frequency `1254.000 MHz`, `M=4095`, `F=0`, `N=76`, `DIVA=4`, injection `Low`.
- Staged LO2 fields: frequency `3934.913 MHz`, `M=4062`, `F=2518`, `N=59`, `DIVA=1`, injection `High`.
- Expected LO1 dirty writes: `R4 0x63AE81DC`, `R0 0x00260000`.
- Expected LO2 dirty writes: `R1 0x40017EF1`, `R0 0x001DCEB0`.

## Implementation Map

Phase 1: `saTech` session core

- Add a `satech_session.py` module.
- Implement CP2102N port discovery.
- Implement persistent serial open and close.
- Implement startup drain and ID verification.
- Implement locked command send/read.
- Implement report parsing into `values`, `checks`, and `raw_lines`.

Phase 2: `saTech` MCP surface

- Add `satech_mcp_server.py`.
- Expose `satech_list_ports()`.
- Expose `satech_open_session()`.
- Expose `satech_close_session()`.
- Expose `satech_session_status()`.
- Expose `satech_command()`.
- Expose typed wrappers for `id`, `RFin`, `fulltest plan`, and `fulltest program`.

Phase 3: expected-write builder

- Add helpers that convert staged FMN fields into expected MAX2871 dirty writes.
- Keep the helper independent of the Rigol capture logic.
- Add focused unit tests for known cases:
  - `Verify 20`: LO1 `R0 0x001B8000`, LO2 `R1 0x400147A9`, `R0 0x001DA1A8`.
  - Prior `Verify 10` history case.
  - Variable-RFin history case with `DIVA=4` and expected `R4`.

Phase 4: coordinator

- Add `verify_frequency(target_mhz, sa_port=None, bk_port=None)`.
- Reuse existing Rigol MCP tools for setup, status, run, and SPI decode.
- Reuse existing BK390A MCP tools for discovery and stable read.
- Return one structured report with clear pass/fail status.

Phase 5: firmware framing improvement

- Add a command completion marker or prompt to `saTech` firmware output.
- Optionally include command echo or request id.
- Update `satech_session.py` to wait for the marker instead of quiet time.

## Storyboard

1. User asks: `Verify 20`.
2. Coordinator opens `saTech`.
3. `saTech` session reports `saTech WN2A ready`.
4. Coordinator resolves and configures Rigol.
5. Coordinator sends `RFin 123.345`.
6. Coordinator sends `fulltest plan 20.000`.
7. Coordinator parses staged LO1 and LO2 fields.
8. Coordinator computes expected dirty writes.
9. Coordinator arms Rigol for LO1.
10. Coordinator confirms `WAIT`.
11. Coordinator sends `fulltest program lo1`.
12. Coordinator confirms `STOP`.
13. Coordinator decodes LO1 SPI.
14. Coordinator compares LO1 expected vs decoded.
15. Coordinator repeats arm, program, stop, decode, compare for LO2.
16. Coordinator reads BK390A.
17. Coordinator converts voltage to dBm.
18. Coordinator closes `saTech`.
19. Coordinator returns one report.

## Commit Proposal

Subject:

```text
Document deterministic saTech verification design
```

Body:

```text
Capture the proposed saTech session server, response model, expected-register
builder, and high-level Verify <MHz> coordinator design.

Include a phased implementation map and storyboard for making the hardware
verification flow deterministic across saTech, Rigol, and BK390A devices.
```
