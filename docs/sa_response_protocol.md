# Spectrum Analyzer Response Protocol: Controller → PC

This document defines the sweep data the spectrum analyzer firmware returns to the PC control/display software over serial. Its companion, `sa_command_protocol.md`, defines the PC → controller direction (sweep requests). The word format and tag tables for both directions are in `sa_word_format.md`.

---

## 1. Goals

1. **Minimize serial traffic.** Below-squelch readings are run-length encoded (RLE). This helps most when zoomed out, where the bandwidth is wide and the peaks are narrow.
2. **Minimize reference-clock switching.** Switching between the two reference clocks is slow, up to 10 ms per switch. Each sweep therefore measures **all ref 1 frequencies first, then all ref 2 frequencies**. The best-spur frequencies for ref 1 and ref 2 are **interlaced** across the band, not contiguous.
3. **Keep the PC in sync without per-point frequency tags.** The PC knows the sweep plan in full (which frequencies are measured, which reference each belongs to, and the order they are sent). It rebuilds the trace from position in the stream.
4. **Support 30 sweeps per second** in continuous mode, and very large sweeps (millions of points, up to the 3,000,001-point hardware maximum) for calibration.

---

## 2. Sweep Structure

```
[ref 1 data words ...] TRAILER(ref1) PTS_COUNT PTS_MULT      <- at ref 1 → ref 2 switch
[ref 2 data words ...] TRAILER(ref2) PTS_COUNT PTS_MULT      <- at ref 2 → ref 1 switch (end of sweep)
```

- **Segments:** each segment covers its reference's frequencies across **all** sub-band entries, in combined-list order (see `sa_command_protocol.md`, section 3.2). There are only two segments and two reference switches per sweep, however many entries the request has.
- **Runs never cross a reference boundary.** When the hardware switches references, the firmware sends any pending run count before the trailer. (The **pending run count** is the number of below-squelch points counted but not yet sent.)
- The trailer is sent **after** its segment, so the firmware knows the real point count by then.
- The ref 2 → ref 1 trailer doubles as the **end-of-sweep marker**. The decoder resyncs at every segment boundary, so an error costs at most one segment.
- The firmware sends the trailer group in the order `TRAILER, PTS_COUNT, PTS_MULT`. The decoder relies only on `TRAILER` coming first. The other two are tagged and can be read in any order.
- Trailer fields carry no segment ID of their own, so they are attributed to the segment closed by the most recent `TRAILER`.
- `PROGRESS` words can appear **anywhere** in the stream, including in the middle of a run (the pending run count is not sent early; it keeps counting). They don't move the cursor.

---

## 3. Reference Assignment

Which reference a frequency uses is **not a formula**. It is set per unit during calibration and stored on the controller, one **Ref flag** per frequency on the 1 kHz grid (0 = ref 1, 1 = ref 2).

The response stream depends on it: the PC can only place a segment's words at the right frequencies if it knows which frequencies belong to that segment. So **the PC must hold the same Ref flags as the unit**, bit for bit. It reads them back from the unit (see `sa_command_protocol.md`, section 5).

- A mismatch that changes how many frequencies each reference gets is caught by the trailer point count.
- A mismatch that **keeps the counts equal** is not. If frequency A moves from ref 1 to ref 2 while frequency B moves the other way, both counts still pass, but every amplitude between A and B is drawn one slot off, in **every** sweep.
- The guard is a **table ID**, which the firmware includes in the request echo (see `sa_command_protocol.md`, section 4). The PC compares it with the ID of the flags it read back before trusting any sweep. The ID covers the unit's whole calibration table, since a recalibration can change the tuning values as well as the Ref flags.

---

## 4. Encoding Rules

1. **Below squelch → fold into the current RLE run**, whatever the lock status. A below-squelch reading is data the user has chosen to ignore, so it gets no error checking.
2. **At or above squelch → send one word:** `AMP_LOCKED` if locked, `AMP_FAILLOCK` if not. In a fast sweep, where lock isn't checked, `AMP_SKIPLOCK`. Any pending run count is sent first.
3. **Run encoding:** a pending run count is sent as `RLE_MULT` (if the run ≥ 4096) and `RLE_COUNT` (if the low 12 bits are nonzero). A run can never be longer than its segment (at most 3,000,001 points), so it always fits in one `RLE_MULT` + `RLE_COUNT` pair and never has to be sent early because of overflow. On the PC, consecutive run words simply add to the cursor.
4. **Reference switch:** send any pending run count, then the trailer group.
5. **Squelch = 0:** no reading is ever below squelch, so no runs are emitted and the real noise floor is returned. This is slower, and the user opts into it.
6. The firmware is responsible for sending the correct number and sequence of words. **The tag sequence matters more than payload correctness**: a bad amplitude payload spoils one point, but a bad tag can break framing. The payloads that affect framing are the **run lengths** (`RLE_COUNT`, `RLE_MULT`), which is why the trailer point count exists. A bad `RLE_MULT` is the most damaging, since it shifts the cursor in steps of 4096.

**Below squelch, lock status is a don't-care.** A less-than-1 dB error from a failed lock doesn't matter to an ignored reading, and the planned fast sweep (which catalogues interesting signals without waiting for lock) relies on the same reasoning.

### Calibration example

A perfect 3,000,000-point calibration sweep (everything below squelch) produces about 4 run words, two per segment, plus the two 3-word trailers: 10 words, 20 bytes. Sending every point individually would take about 6 MB.

---

## 5. Progress

`PROGRESS` (`0x0F`) is both a liveness pulse and a progress report: each word carries an incrementing sequence number.

- **Triggered by point count,** not by a timer: one `PROGRESS` after every *spacing* measured points, set by the `PROGRESS` command word in the sweep request (`sa_command_protocol.md`, section 3). Spacing `0` means off.
- **The point counter runs continuously** across both reference segments.
- **Payload: a 12-bit sequence number,** which resets to 0 at the start of each sweep.
- **It never wraps:** spacing must be ≥ ⌈N_total / 4096⌉ and ≤ 4095 so the sequence always fits in 12 bits. For the hardware-maximum sweep (3,000,001 points) the minimum spacing is **733**, which yields 4,092 beats (sequence 0–4091) plus a 565-point remainder with no final extra beat.
- **The interval follows the point rate.** Serial traffic stretches it, and a reference switch adds its full duration to the beat that spans it. The PC's watchdog allowance must cover both.

---

## 6. What the Checks Detect

| Check | Failure means |
| --- | --- |
| Cursor ≠ catalog count | Transport problem: a lost word, or a corrupted run length. |
| Firmware point count ≠ catalog count | PC and firmware disagree about the sweep plan or the Ref flags. |

**Overrun** is detected in two places. An amplitude word that arrives when the cursor is already at the end would write past the trace, so it is caught on arrival. A run word that pushes the cursor past the end writes nothing, so it is caught when the run is applied.

**How a corrupted RLE_MULT is detected.** A bad payload can't be recognized from the word itself, since any 12-bit value is a legal multiplier. It shows up in one of two ways:

- **Too large:** the cursor overshoots, which is an overrun.
- **Too small:** the segment comes up short by a multiple of 4096 points, and the count checks fail.

So **a segment that contained any RLE_MULT word and fails a count check is treated as a possible corrupted RLE_MULT** (section 7). The decoder can't tell which word was bad, so a failure caused by some other word in that segment is treated the same way.

**No checksum.** The design deliberately has none. A 12- or 13-bit check over long, dense segments can only offer a statistical miss rate of roughly 1 in 4,096 to 1 in 8,192, which wasn't worth a trailer word. Integrity relies on the count checks, the per-point status messages, the PC's live log, and the technician's judgment when reading the plots. What goes undetected:

- **Corrupted amplitude values.** These spoil one point each and are visible on the plot.
- **Errors whose cursor effects cancel.** For example, one RLE count corrupted upward by _d_ and another in the same segment corrupted downward by _d_: the cursor still ends on the right total, but the points between the two are shifted by _d_. This needs two independent errors of exactly opposite size in one segment, so it is very rare.

A Ref flag mismatch that keeps the counts equal would also pass the count checks. That one is guarded separately, by the table ID in the request echo (section 3).

---

## 7. Error Policy

**All bad data is sent to the PC, plotted, and possibly logged. A sweep is not plotted in only two cases: an overrun, or a failed segment that contained RLE_MULT.**

| Condition | Action |
| --- | --- |
| Overrun: the point count exceeds the expected count (run overshoot or extra amplitude) | **Drop the sweep and don't plot it.** The screen may flicker; that's acceptable. |
| A count check fails in a segment that contained an RLE_MULT word (possible corrupted RLE_MULT) | **Drop the sweep and don't plot it.** |
| Short count (no RLE_MULT in the segment) | Plot, light the red LED, log. |
| Firmware/catalog point-count mismatch (no RLE_MULT in the segment) | Plot, light the red LED, log. |
| AMP_FAILLOCK | Plot as a normal amplitude and log the frequency. The user can review a log of every frequency that failed to lock. |
| AMP_SKIPLOCK | Plot as a normal amplitude. Lock wasn't checked (fast sweep), so nothing is logged. |
| Lock failure below squelch | Not reported; it is folded into the RLE run. |
| No `PROGRESS` within interval × allowance | Show "unit not responding." |
| `PROGRESS` sequence gap | Log the missed beats. The unit is alive, so no other action. |

---

## 8. Open Items

- **Spare status `0x0E`** (echo and reject occupy `0x0C` and `0x0D`).

### For consideration in future updates

- **Cancelling run-count errors.** The PC only checks where the cursor ends up, and some combinations of errors land it in the right place by accident. For example, in one segment:
  - One RLE_COUNT gets corrupted from 40 to 45 (+5).
  - A later RLE_COUNT gets corrupted from 30 to 25 (−5).

  The cursor still finishes on exactly N, so both count checks pass. But every amplitude between those two run words is drawn 5 frequencies too high. A lost amplitude word combined with a run count corrupted up by 1 has the same effect. It takes two independent errors of exactly opposite size in the same segment, so it's very rare. Without a checksum, nothing on the wire catches it. Only the plot will show it.
