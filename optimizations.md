# Sweep Optimizations

## Goal

Minimize communication and computation before the synthesizers can settle and lock. The optimized code does not replace the required lock, analog-settling, or ADC-acquisition time; it reduces the firmware overhead added to those times.

## Sweep organization

- Run the sweep autonomously on the Arduino.
- Use 401 points from 0 to 3,000,000 kHz in 7,500 kHz steps for the current performance test.
- Do not print during the sweep. Print one report after the sweep completes.
- Measure performance over the complete sweep and report totals and per-point averages. Individual points have different costs because the number of changed registers varies.
- When the complete acquisition path exists, accumulate these phases separately across the sweep:
  1. Divider retrieval or calculation and synthesizer programming.
  2. PLL lock acquisition.
  3. Required post-lock analog settling.
  4. ADC conversion and result transfer.

## Synthesizer programming

- Use the MAX2871 driver's shadow registers and dirty-register writes. Only registers whose values changed are sent.
- Do not program LO3 during this sweep.
- Program LO2 at every point.
- Program LO1 only when its divider state changes. For the current table, `F=0` and `M=2` remain constant, but both `N` and `DIVA` must be checked.
- The 401-point sweep requires 46 LO1 updates instead of 401.
- Precompute the MAX2871 divider values so the timed sweep does not run `freq2FMN()` at every point.

## Divider storage and buffering

- Keep the LO1 table in program memory, including its varying `N` and `DIVA` values.
- Store the LO2 sweep in the external IS66 serial PSRAM for the current buffered test.
- Pack each LO2 point into one 32-bit word:

  ```text
  [ FRAC:12 ][ M:12 ][ N:8 ]
  ```

- LO2 uses `DIVA=0` throughout the current sweep, so DIVA is supplied as a constant rather than stored in each word.
- Read eight packed LO2 words per IS66 transaction into a 32-byte Arduino SRAM buffer. This amortizes the SPI command and address overhead.
- Use the IS66 serial read command `0x03` and serial write command `0x02` with a 24-bit byte address at the existing 16 MHz RAM SPI clock.
- Loading the table into the IS66 occurs before timing begins. The measured precomputed time includes the buffered IS66 reads but not initial table loading.
- The test loads the IS66 directly with raw SPI because the current IS66 driver only implements device identification.
- The eight-word buffer raises static SRAM use from 737 to 769 bytes out of 2,048 bytes.

## Measured progression

| Implementation | Total | Time per point | Rate |
| --- | ---: | ---: | ---: |
| Calculate and program all three LOs | 675,544 us | 1,684 us | 593 points/s |
| PROGMEM dividers, program all three LOs | 281,120 us | 701 us | 1,426 points/s |
| Calculate, omit LO3, update LO1 sparsely | 319,352 us | 796 us | 1,255 points/s |
| PROGMEM dividers, omit LO3, update LO1 sparsely | 175,476 us | 437 us | 2,285 points/s |
| Eight-word IS66 buffer, omit LO3, update LO1 sparsely | 161,900 us | 403 us | 2,476 points/s |

The current implementation reduces firmware and programming overhead by about 76% compared with calculating and programming all three LOs.

The final Uno test build used 769 of 2,048 SRAM bytes and 19,482 of 32,256 flash bytes.

## Reconstructing the test

- Generate one divider tuple per point by applying the current frequency plan and `freq2FMN()` implementation to the 401 sweep frequencies.
- Store one LO1 tuple per point as `{FRAC, M, N, DIVA}` and program LO1 only when `N` or `DIVA` differs from the preceding point.
- Pack each LO2 tuple into the 32-bit layout above. The tested sweep had `DIVA=0` for every LO2 point.
- Before timing, write the packed LO2 table sequentially to IS66 address zero.
- During timing, refill the eight-word SRAM buffer whenever the point index is divisible by eight.
- Time the complete 401-point loop with `micros()` and print only after the loop.

## Memory roles

- The IS66 is 32-Mbit volatile serial PSRAM used as fast working memory.
- The W25N01GV is nonvolatile SPI NAND flash, not SDRAM.
- The current W25N driver implements identification and status access; its NAND page, bad-block, and result-storage behavior is unfinished.
- ADC-result buffering in NAND and final result-output optimization remain future work and are not part of the measurements above.

## Measurement limits

- The current benchmark measures divider handling and synthesizer programming.
- It does not wait for or verify PLL lock.
- It does not include post-lock analog settling.
- It does not acquire or store ADC samples.
