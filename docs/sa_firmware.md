# Spectrum Analyzer Controller Firmware

This document describes how the controller firmware implements the serial protocols: reference switching, the LO2 calibration table, the sweep loop and progress reporting. The protocols themselves are defined in `sa_word_format.md`, `sa_command_protocol.md` and `sa_response_protocol.md`.

---

## 1. Reference Switching

| Ref | Part | Frequency | Start-up time | Disabled (tri-state pin low) |
| --- | --- | --- | --- | --- |
| Ref 1 | Ecliptek EC2600TS-66.000M | 66.000 MHz | 10 ms max | Oscillator stopped, 10 µA max standby |
| Ref 2 | Abracon ASFL1-66.666MHZ-EC-T | 66.666 MHz | 3 ms typ, 10 ms max (30–79.9 MHz band) | Oscillator stopped, 10 µA max disable current |

**Why switching is slow:** the firmware selects a reference by enabling one oscillator and disabling the other. Pulling the tri-state pin low **stops the oscillator entirely**, which keeps spurious content down. The cost is that every switch, in either direction, is a cold start of up to 10 ms before the PLLs can even begin to relock.

**Procedure:**
1. Switch references (`ref_LO()` / `ref_HI()`), using break-before-make: disable the active oscillator before enabling the other.
2. Call `ref_settle()`. It polls for lock by **toggling the LO between two related frequencies**. Each retune forces a fresh lock acquisition, so a lock-detect left over from before the switch can't satisfy the loop; lock-detect only goes high once the new reference is running.
3. Exit on the first verified lock, or at the timeout.
4. Tune to the segment's first target frequency as normal. The per-point lock check then sets the 4-bit message (AMP_LOCKED or AMP_FAILLOCK) as for any other point.

`ref_settle()` returns nothing. If it times out, the normal per-point flags show the consequence. A reference that never settled shows up as a series of AMP_FAILLOCK entries, which the technician reviews in the log.

```cpp
// After ref_LO() / ref_HI(): wait for a verified lock, or give up at the timeout.
void ref_settle(uint32_t f_a, uint32_t f_b, uint32_t timeout_us)
{
    uint32_t t0 = micros();
    bool use_a = true;
    while ((micros() - t0) < timeout_us) {
        program_lo(use_a ? f_a : f_b);           // freq2fmn + register writes
        uint32_t t1 = micros();
        while ((micros() - t1) < PLL_Lock_timeout) {
            if (digitalRead(SA.PLL_MUX))
                return;                          // fresh lock after a retune
        }
        use_a = !use_a;                          // no lock yet: retune and retry
    }
}
```

- **Toggle frequencies:** the segment's first point and its neighbor. The loop ends near the target, so the final hop is small.
- **Timeout (`ref_settle_us`):** calibrated per unit and stored in the calibration header (section 2.1). Before calibration, use **12 ms** (10 ms worst-case oscillator start-up plus relock margin).
- **Calibration procedure:**
  1. With the unit otherwise calibrated (or using provisional dividers), run many ref1↔ref2 switches through the toggle-lock `ref_settle` loop.
  2. Record time from the switch to the first verified lock on each trial.
  3. Set `ref_settle_us` to the **maximum observed time plus a fixed margin** (margin chosen on the bench so rare slow starts still settle inside the timeout).
  4. Write `ref_settle_us` into the calibration header with the CRC-32.
- **Optional stricter check:** if a single lock proves premature on the bench (the PLL locking while the oscillator is still pulling onto frequency), require two consecutive locks, one on each toggle frequency, before exiting.
- **If spurious content appears at the switch,** fall back to quietly waiting out the calibrated delay instead of toggling.

---

## 2. LO2 Calibration Table

The unit's reference assignment (`sa_response_protocol.md`, section 3) is stored in the **LO2 calibration table** in the controller's **W25N01GV** SPI NAND flash. Each entry holds LO2's precomputed MAX2871 divider values for one frequency, plus the Ref flag saying which reference that frequency was calibrated against.

**Entry format (one 32-bit word):**

| Bits | Field |
| --- | --- |
| 31–20 | F (fractional, 12 bits) |
| 19–8 | M (modulus, 12 bits) |
| 7 | **Ref:** 0 = ref 1, 1 = ref 2 |
| 6–0 | N (integer, 7 bits) |

**Bit 7 is repurposed.** `freq2fmn()` and the MAX2871 driver use an 8-bit N field (`[FRAC:12][M:12][N:8]`). For LO2, N never exceeds 127, so N's bit 7 is always 0 and is free to carry the Ref flag:

- **Calibration** writes the Ref flag into bit 7 when it builds the table.
- **The firmware must clear bit 7 before handing N to the MAX2871 driver** (`N = lo2 & 0x7F`). Otherwise a ref 2 frequency would be programmed with N + 128.

Also:

- LO2's output divider is a constant ÷1 for this instrument (register value **DIVA = 0**), so it isn't stored.
- The reference reaches the PFD undivided, so with LO2's VCO at 3–6 GHz and a reference of about 66 MHz, N stays within about 45–91, inside 7 bits.

### 2.1 Calibration header

Page 0 of the W25N01GV holds a small **calibration header**. The LO2 table begins at page 1 so index 0 does not overwrite the header.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | `magic` — `0x53414C32` (`SAL2`) |
| 4 | 4 | `version` — header format version (`1`) |
| 8 | 4 | `crc32` — IEEE/zlib CRC-32 of the LO2 table bytes (little-endian) |
| 12 | 4 | `ref_settle_us` — calibrated `ref_settle()` timeout (little-endian) |

Remaining bytes in page 0 are reserved (write as 0xFF). Firmware loads `crc32` and `ref_settle_us` from this page at start-up and for `REF_FLAGS` read-back.

**Table layout:**

| Property | Value |
| --- | --- |
| Resolution | 1 kHz |
| Entries | 3,000,001 (0–3,000,000 kHz) |
| Index | the frequency in kHz |
| Size | 4 bytes per entry: 12,000,004 bytes |
| Base address | 2048 (block 0, page 1, byte 0) |
| Entries per page | 512 (no entry crosses a page boundary) |

**Addressing uses only shifts, ORs and masks:**

```c
uint32_t addr   = TABLE_BASE | (index << 2);   /* TABLE_BASE = 2048; 4 bytes per entry */
uint16_t page   = addr >> 11;                   /* 2,048-byte pages */
uint16_t column = addr & 0x7FF;                 /* byte within the page */
```

Because the table is at 1 kHz resolution, **a request's start kHz is the table index and its step kHz is the stride.** The firmware never divides.

**Lookup.** Reading an entry gives the firmware both what it needs to tune LO2 and which reference that frequency uses. The Ref flag comes free with the read the firmware has to do anyway. Each segment walks the combined frequency list in order, tunes the entries whose Ref bit matches the segment's reference, and skips the rest.

- Use the W25N01GV's **own 2,048-byte page buffer** as the cache. Page Data Read (13h) copies a page from the NAND array into the chip's buffer (labeled about 50 µs in Winbond's datasheet), and Read Data (03h) then clocks out only the 4-byte entry needed, starting at its column address. No AVR RAM is needed for the cache.
- A new page is needed once per 512 entries at stride 1, once per about 102 points at stride 5, and every point at strides of 512 or more. Even the worst case adds only about 50 µs per point, which matters mainly in fast sweep, where there's no lock wait.
- Measure the actual page-load time on the bench; the ~50 µs figure is from the datasheet's operation-flow diagram.

**Ref flag read-back.** For the read-back command (`sa_command_protocol.md`, section 5), the firmware sends the header's `crc32` (4 bytes, little-endian), then packs bit 7 of each entry into the 375,001-byte flag stream. The CRC-32 is computed at calibration time and stored in the header with `ref_settle_us`.

---

## 3. Sweep Loop

This is a single loop with streaming output. The tagged word is built for every point, including below-squelch points where it is simply discarded, so the lock-status logic never needs to know about squelch. No buffer is needed: the pending state is the run counter, and a word is sent only once its fate is decided.

```c
#include <stdint.h>

enum {                                      /* response tags, sa_word_format.md section 3 */
    TAG_NONE          = 0x00,
    TAG_AMP_LOCKED    = 0x01,               /* 0x1-0x3: amplitudes */
    TAG_AMP_FAILLOCK  = 0x02,
    TAG_AMP_SKIPLOCK  = 0x03,               /* fast sweep: lock not checked */
    TAG_RLE_COUNT     = 0x04,               /* 0x4-0x7: runs */
    TAG_RLE_MULT      = 0x05,
    TAG_TRAILER       = 0x08,               /* 0x8-0xB: trailer */
    TAG_PTS_COUNT     = 0x0A,
    TAG_PTS_MULT      = 0x0B,
    TAG_ECHO          = 0x0C,               /* 0xC-0xF: status */
    TAG_REJECT        = 0x0D,
    TAG_PROGRESS      = 0x0F
};

#define WORD(tag, val) ((uint16_t)(((uint16_t)(tag) << 12) | ((uint16_t)(val) & 0x0FFF)))

static void send_run(uint32_t run)
{
    if (run >> 12)    serial_send16(WORD(TAG_RLE_MULT,  run >> 12));  /* bits 12-23 */
    if (run & 0x0FFF) serial_send16(WORD(TAG_RLE_COUNT, run));        /* bits 0-11  */
}

typedef struct { uint32_t start_khz, step_khz, n; } entry_t;   /* one sub-band entry */

/* Walks the combined frequency list (entry 0, entry 1, ...) and measures only
 * the frequencies that belong to this segment's reference. */
void sweep_segment(uint8_t seg_id, const entry_t *entries, uint8_t n_entries, uint16_t squelch)
{
    uint32_t run = 0;                       /* up to 3,000,001 */
    uint32_t n_points = 0;                  /* points measured in this segment */

    for (uint8_t e = 0; e < n_entries; e++) {
        for (uint32_t j = 0; j < entries[e].n; j++) {   /* 32-bit: up to 3,000,001 points */
            uint32_t index = entries[e].start_khz + j * entries[e].step_khz;   /* kHz = table index */
            uint32_t lo2   = read_lo2_entry(index);     /* 32-bit entry from flash, section 2 */
            if (((lo2 >> 7) & 1) != seg_id) continue;   /* Ref bit: 0 = ref 1, 1 = ref 2 */

            uint16_t adc  = measure_point(lo2 & ~0x80UL);   /* clear Ref (bit 7), program LO2, settle, read ADC */
            n_points++;
            progress_poll();                            /* section 4; never touches the run */
            uint16_t word = WORD(pll_locked() ? TAG_AMP_LOCKED : TAG_AMP_FAILLOCK, adc);

            if (adc < squelch) {            /* tag discarded, lock status irrelevant */
                run++;                      /* can't exceed the segment length (≤ 3,000,001) */
                continue;
            }
            if (run) {                      /* close out the pending run */
                send_run(run);
                run = 0;
            }
            serial_send16(word);
        }
    }

    if (run) send_run(run);                /* send any pending run count at reference switch */

    serial_send16(WORD(TAG_TRAILER,   seg_id));
    serial_send16(WORD(TAG_PTS_COUNT, n_points));        /* bits 0-11  */
    serial_send16(WORD(TAG_PTS_MULT,  n_points >> 12));  /* bits 12-23 */
}

/* One sweep (single range = one entry; sub-bands = several):
 *   progress_start(spacing_from_request);
 *   ref_LO(); ref_settle(f1_first, f1_next, settle_us);
 *             sweep_segment(SEG_REF1, entries, n_entries, squelch);
 *   ref_HI(); ref_settle(f2_first, f2_next, settle_us);
 *             sweep_segment(SEG_REF2, entries, n_entries, squelch);
 *   ref_LO()  (ready for next sweep; settled at the start of the next one)
 *
 *   f1_first / f2_first = the first ref 1 / ref 2 frequency in the combined list.
 *   settle_us = header ref_settle_us (12 ms before calibration).
 *   SEG_REF1 = 0, SEG_REF2 = 1, so seg_id matches the entry's Ref bit.
 */
```

Notes:
- Every counter of points (`j`, `index`, `n_points`, the run counter) is **32-bit**.
- On AVR, `tag << 12` is just a nibble placed in the high byte, so building the word costs a few cycles per point. That is nothing next to PLL settle time.

---

## 4. Progress

Progress is defined in `sa_response_protocol.md`, section 5. The firmware side is a point counter with no timer or drift logic:

```c
static uint16_t progress_spacing, progress_count, progress_seq;   /* spacing = 0: off */

void progress_start(uint16_t spacing)          /* call at sweep start */
{
    progress_spacing = spacing;
    progress_count   = 0;
    progress_seq     = 0;
}

void progress_poll(void)                       /* call once per measured point */
{
    if (progress_spacing && ++progress_count >= progress_spacing) {
        serial_send16(WORD(TAG_PROGRESS, progress_seq++));
        progress_count = 0;
    }
}
```

- `progress_poll()` runs once per **measured** point, so the counter carries across both reference segments.
- `PROGRESS` words go out without disturbing the run counter, so a beat can fall in the middle of a run.

### 4.1 Measured point rates (bench)

Technician command `pointrate` (tag `measure-sweep-rate`) times the acquisition path over 100 RF points starting at 10 MHz with 7.5 MHz steps: sparse LO1, LO2 every point, ADC1. Normal waits for LO1 then LO2 lock (500 µs timeout each); fast skips lock.

| Mode | Points | Total | Rate |
| --- | ---: | ---: | ---: |
| Normal (lock + ADC) | 100 | 145,196 µs | **688 points/s** |
| Fast (ADC, no lock wait) | 100 | 92,260 µs | **1083 points/s** |

The PC uses these rates to choose `PROGRESS` spacing (`sa_pc_software.md`, section 4.1). Re-run `pointrate n` / `pointrate f` after measure-path changes.

---

## 5. Open Items

- **Bad blocks:** how the LO2 calibration table is placed and kept in good blocks of the W25N01GV (the table spans about 92 of its 128 KB blocks; page 0 is the calibration header).
- **Page-load time** of the W25N01GV, measured on the bench.
