# Full Test

## Purpose

This test derives the frequency plan from a given `RFin` value and verifies that the LO chain is programmed and measured correctly as a coordinated RF path. Because the LO select pins are automatically controlled during SPI programming and may transition too quickly to observe with `digitalRead`, the relevant select and control pins must be checked separately before programming. The result must include separate technician-facing reports for each LO and the final amplitude check for the `315 MHz` path.

## Architecture

This full test should run while the technician console firmware is loaded so the same firmware image can support both automated test execution and manual technician troubleshooting. The technician console should remain the operator-facing interface for board-side actions such as setting `RFin`, selecting or verifying control states, programming the LOs, and reporting board status in a form that is useful to the technician. This avoids requiring a separate firmware build or reprogramming step when the technician needs to stop, inspect the board state, or perform follow-up checks after an automated run.

The full test itself should be orchestrated by host-side code that uses the technician console as its control surface for the board. The host should send the required technician-console commands in a stable and parseable form, sequence the LO programming steps, and collect the board-side responses. The same host code should also handle the external instrument work that does not belong in the firmware: decode the oscilloscope capture for each LO programming event, compare the decoded register values against `max2871_expected.py`, read the BK390A voltage directly, convert that voltage to dBm, and collate the final result into separate technician-facing LO reports plus the overall `315 MHz` path amplitude result.

## Acceptance Checklist

- Frequency plan report produced from `RFin`, showing the planned `LO1` and `LO2` frequencies and each LO's `M`, `F`, `N`, and `DIVA` values.

## Report Formats

Frequency plan report:

```text
Frequency Plan Report
RFin: 1735.113 MHz
IF1: 3590.887 MHz
IF2: 315.000 MHz

LO1:
  frequency: 5326.000 MHz
  M: 80
  F: 45
  N: 4095
  DIVA: 1

LO2:
  frequency: 3905.887 MHz
  M: 59
  F: 1234
  N: 4095
  DIVA: 1
```

## Procedure

The procedure is:

- Take `RFin`.
- Derive the frequency plan.
- Run a separate pin-state check before programming.
- Program `LO1`.
- Decode the scope's 32-bit register output for `LO1`.
- Compare the decoded values against `max2871_expected.py`.
- Program `LO2`.
- Decode the scope's 32-bit register output for `LO2`.
- Compare the decoded values against `max2871_expected.py`.
- Read the BK390A voltage directly.
- Convert the voltage to dBm.
- Report the amplitude check for the `315 MHz` path.
- Present separate reports for `LO1` and `LO2`.

## Voltage to dBm

The notes on converting voltage to dBm remain as follows:

        def _volts_to_dBm(self, voltage: float) -> float:
            '''
            Convert ADC results from Volts to dBm for the y_axis

            @param voltage Derived from the number of ADC bits and the uController voltage reference
            @type float
            @return Output power in the range of -80 to +20 dBm
            @rtype float
            '''
            x = voltage
            # Polynomial derived from a graph found in the LogAmp spec sheet.
            dBm = (((((((-9.460927*x + 110.57352)*x - 538.8610489)*x + 1423.9059205)*x - 2219.08322)*x + 2073.3123)*x - 1122.5121)*x + 355.7665)*x - 112.663
            return dBm
