# Sweep Steps

## Single-point foundation

- [ ] Define one production measurement command and response.
- [ ] Update the binary command executor to handle that command correctly.
- [ ] Program the required LOs and attenuator.
- [ ] Wait for PLL lock and the required analog settling time.
- [ ] Read one ADC sample.
- [ ] Return the frequency and measurement with an unambiguous completion boundary.
- [ ] Repeat the transaction reliably before implementing sweep playback.

## Complete sweep playback

- [ ] Convert a start-stop-step request into fixed-size internal instruction records.
- [ ] Stop the current sweep before replacing its instruction sequence.
- [ ] Write the complete instruction sequence to IS66 SerialRAM from address zero.
- [ ] Read and execute the stored instructions sequentially through the shared command path.
- [ ] Wrap to the first instruction and repeat the sweep continuously.
- [ ] Stop playback and replace the stored sequence when a new sweep request arrives.
