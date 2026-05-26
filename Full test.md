1. It seems that we can correctly decode the 32-bit registers from the oscilloscope.
2. The MCP server also has the ability to report the assert/deassert status of the pin for the selected lo.
3. We can also verify any of the control pins whether they are asserted or deasserted
4. The AI should also know the frequency plan so if I give it an RFin value it can:
    - Select each lo
    - program the selected lo
    - Then report:
        + Which Lo was programmed
        + The Lo select pin was properly asserted
        + Lo frequency that was programmed
        + Lo register values that were programmed
        + If the register values were correct
    - Read the meter to report the logamp voltage

5. As steps:
    - Get the freq plan from the RFin value
    - For Lo 1 and 2:
        + Assert & verify 'select' pin (digitaRead)
        + Deassert & verify 'select' pin
        + Get the selected Lo's expected register values based on RFin and frequency plan
        + Program selected Lo to frequency based on RFin and frequency plan
        + Decode 32-bit register values from the o'scope
        + Compare the 32-bit decoded values against the expected values
        + Read the LogAmp voltage using the BK390A meter
        + Convert voltage to dBm:

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
