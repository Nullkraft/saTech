This test derives the frequency plan from a given `RFin` value and verifies that the LO chain is programmed and measured correctly as a coordinated RF path. Because the LO select pins are automatically controlled during SPI programming and may transition too quickly to observe with `digitalRead`, the relevant select and control pins must be checked separately before programming. The result must include separate technician-facing reports for each LO and the final amplitude check for the `315 MHz` path.

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
