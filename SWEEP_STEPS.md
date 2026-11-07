# Sweep Steps

## Single-point foundation

- [ ] Limit the initial measurement path to LO1, LO2, and ADC1; defer LO3, ADC2, fine tuning, calibration, and linearity.
- [ ] Define one production measurement command and response.
- [ ] Update the binary command executor to handle that command correctly.
- [ ] Program LO1, LO2, and the attenuator.
- [ ] Select digital lock detect for the PLL being checked and tri-state the other PLL's MUXOUT.
- [ ] Poll the wired-OR MUXOUT input on A2 for no more than 500 us.
- [ ] Read the raw 16-bit ADC1 SPI response immediately when lock is detected.
- [ ] If lock times out, still read ADC1 and report failure-to-lock with the raw response.
- [ ] Return the frequency, raw ADC1 response, and lock status with an unambiguous completion boundary.
- [ ] Repeat the transaction reliably before implementing sweep playback.

## Complete sweep playback

- [ ] Convert a start-stop-step request into fixed-size internal instruction records.
- [ ] Stop the current sweep before replacing its instruction sequence.
- [ ] Write the complete instruction sequence to IS66 SerialRAM from address zero.
- [ ] Read and execute the stored instructions sequentially through the shared command path.
- [ ] Wrap to the first instruction and repeat the sweep continuously.
- [ ] Stop playback and replace the stored sequence when a new sweep request arrives.
