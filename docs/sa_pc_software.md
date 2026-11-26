# Spectrum Analyzer PC Software

This document describes how the PC control/display software implements the serial protocols: building the sweep plan, decoding the response stream, display, the progress watchdog, live logging and the red LED. The protocols themselves are defined in `sa_word_format.md`, `sa_command_protocol.md` and `sa_response_protocol.md`.

---

## 1. Sweep Plan

The plan is built once per configuration. It is rebuilt only when the sweep request or the unit's Ref flags change, never per frame.

- Frequencies are **integer kHz**, never floats, matching the controller. Floats would make lookups fragile.
- The plan's frequency list is the **combined list** from the request: entry 0's points, then entry 1's, and so on (`sa_command_protocol.md`, section 3.2). That is the order the firmware walks within each segment, so the decoder needs nothing extra for sub-bands.
- `entry_bounds` records where each entry starts in the combined list. **The PC keeps track of the sub-bands this way** for later plotting.
- `ref2_flags` is the PC's copy of the unit's **Ref flags** (`sa_response_protocol.md`, section 3), read back once per program run with the CRC-32 table ID and unpacked to one bool per kHz (`True` = ref 2).
- `seg_idx[0]` and `seg_idx[1]` are lookup tables from stream position to display position, one per segment. They handle interlaced references automatically, so decoded values land directly in frequency order. **No per-frame zip, merge, or sort is needed.**

```python
import numpy as np

class SweepPlan:
    def __init__(self, entries, ref2_flags):
        """entries: list of (start_khz, step_khz, n_points); one entry for a single range."""
        self.entries = list(entries)
        self.freqs = np.concatenate([start + step * np.arange(n, dtype=np.int64)
                                     for start, step, n in self.entries])
        # Where each entry starts in the combined list (for plotting sub-bands)
        self.entry_bounds = np.cumsum([0] + [n for _, _, n in self.entries])
        is_ref1 = ~ref2_flags[self.freqs]            # Ref flag lookup, bool array
        # Display positions for each segment, in firmware transmission order
        # e.g. seg_idx[0] = [0, 2, 3, 7, 9, ...]  (interlaced is fine)
        self.seg_idx = [np.flatnonzero(is_ref1), np.flatnonzero(~is_ref1)]
        self.n = len(self.freqs)
```

The read-back flags arrive packed (`sa_command_protocol.md`, section 5) and unpack in one step:

```python
ref2_flags = np.unpackbits(packed, bitorder='little')[:3_000_001].astype(bool)
```

---

## 2. Decoder

- The amplitude array is pre-filled with **0**, which serves as the below-squelch flag. ADC values are raw, and any squelched reading is below threshold anyway, so 0 is unambiguous for display.
- An RLE_COUNT or RLE_MULT word just advances the cursor, because the points it covers are already flagged.
- An AMP_LOCKED, AMP_FAILLOCK or AMP_SKIPLOCK word is written straight to its display slot through `seg_idx`.
- The checks and the drop rules follow `sa_response_protocol.md`, sections 6 and 7.

```python
NONE          = 0x00   # response tags, sa_word_format.md section 3
AMP_LOCKED    = 0x01   # 0x1-0x3: amplitudes
AMP_FAILLOCK  = 0x02
AMP_SKIPLOCK  = 0x03
RLE_COUNT     = 0x04   # 0x4-0x7: runs
RLE_MULT      = 0x05
TRAILER       = 0x08   # 0x8-0xB: trailer
PTS_COUNT     = 0x0A
PTS_MULT      = 0x0B
ECHO          = 0x0C   # 0xC-0xF: status
REJECT        = 0x0D
PROGRESS      = 0x0F
DATA_TAGS = (AMP_LOCKED, AMP_FAILLOCK, AMP_SKIPLOCK, RLE_COUNT, RLE_MULT)
TRAILER_FIELDS = {PTS_COUNT: "count", PTS_MULT: "mult"}

def decode_sweep(words, plan):
    """Returns (amps, issues, faillock_freqs).
    amps is None if the sweep must be dropped."""
    amps = np.zeros(plan.n, dtype=np.uint16)         # 0 = below squelch
    issues, faillock_freqs = [], []
    seg, i, had_mult = 0, 0, False
    groups = []                                      # one trailer group per closed segment

    for w in words:
        tag, val = w >> 12, w & 0x0FFF

        if tag in DATA_TAGS:
            if seg >= len(plan.seg_idx):
                return None, ["data after end of sweep"], []
            idx = plan.seg_idx[seg]
            if tag in (RLE_COUNT, RLE_MULT):
                if tag == RLE_MULT:
                    i += val * 4096
                    had_mult = True
                else:
                    i += val
                if i > len(idx):
                    return None, [f"seg {seg}: RLE overrun"], []
            else:
                if i >= len(idx):
                    return None, [f"seg {seg}: amplitude overrun"], []
                amps[idx[i]] = val
                if tag == AMP_FAILLOCK:
                    faillock_freqs.append(int(plan.freqs[idx[i]]))
                i += 1

        elif tag == TRAILER:
            groups.append({"seg": seg, "id": val, "cursor": i, "had_mult": had_mult})
            seg, i, had_mult = seg + 1, 0, False   # resync at the boundary

        elif tag in TRAILER_FIELDS and groups:
            groups[-1][TRAILER_FIELDS[tag]] = val

        # PROGRESS is handled by the watchdog (section 4) and ignored here.
        # NONE, ECHO, REJECT, and other status codes: handle/log as needed

    for s in range(len(plan.seg_idx)):
        expected = len(plan.seg_idx[s])
        if s >= len(groups):
            issues.append(f"seg {s}: no trailer received")
            continue
        g = groups[s]
        seg_issues = []
        if g["cursor"] != expected:
            seg_issues.append(f"seg {s}: short by {expected - g['cursor']} points")
        if "count" in g and "mult" in g:
            total = g["mult"] * 4096 + g["count"]
            if total != expected:
                seg_issues.append(f"seg {s}: firmware measured {total}, catalog expects {expected}")
        else:
            seg_issues.append(f"seg {s}: point count missing")

        if seg_issues and g["had_mult"]:             # possible corrupted RLE_MULT
            return None, seg_issues + [f"seg {s}: contained RLE_MULT -> sweep dropped"], []
        issues += seg_issues

    return amps, issues, faillock_freqs
```

---

## 3. Display

- Amplitudes are converted from ADC values to dBm on the PC.
- A **GUI checkbox** selects how below-squelch points (value 0) are drawn: a **flat** noise floor at the squelch level, or a **simulated** noise floor.
- Setting squelch to 0 returns the real noise floor, at the cost of speed.
- A full-scale ADC reading (4095) never occurs in normal use. The input is kept well below full scale for best SFDR, and a digital attenuator plus a front-panel max-input caution label protect the front end.
- What the PC shows while waiting for a long sweep (busy cursor, Larson scanner, progress bar) is up to the PC (section 4).

---

## 4. Progress

Progress is defined in `sa_response_protocol.md`, section 5.

### 4.1 Choosing the spacing

Before each sweep:

- Estimate the sweep time as N ÷ the measured point rate for the sweep mode. 732 points/s was the original measured rate without serial traffic; use the current measured rate for each mode.
- Under about 3 s: send a `PROGRESS` spacing of **0 (off)**. This covers normal sweeps up to the recommended 1,600 points.
- Otherwise: send a spacing of about one second's worth of points, **never less than ⌈N_total / 4096⌉**, and **never more than 4095**. For a full-band sweep the minimum is 733. At 732 points/s that is 733 points; a faster mode (a fast sweep that skips the lock wait) gets a proportionally larger spacing.

### 4.2 Progress display

Because the beats are point-based and the sequence never wraps, the sequence number is a direct progress report:

```
fraction = (seq + 1) × spacing / N_total
```

- Estimate the ETA from the **actual beat rate**, not the nominal point rate. The actual rate already includes serial overhead.
- When data is arriving densely, the decoder's cursor also indicates position. Showing the larger of the two keeps the bar smooth.
- The reference switch appears as one slow beat, so the bar pauses briefly at roughly the ref 1 / ref 2 split.

```python
def sweep_progress(seq, spacing, n_total):
    return min(1.0, (seq + 1) * spacing / n_total)
```

### 4.3 Watchdog

The watchdog runs in the serial receive path, on wall-clock time. It is separate from `decode_sweep()`, which ignores `PROGRESS` words. Its expected interval is `spacing` ÷ the measured point rate.

- **No word at all within interval × 1.5 (the drift allowance):** show "unit not responding."
- **Sequence gap:** one or more beats were lost on the link. The unit is still alive, so just log it.

```python
import time

class ProgressWatchdog:
    def __init__(self, interval_s, allowance=1.5):
        self.enabled = interval_s > 0
        self.timeout = interval_s * allowance
        self.last = time.monotonic()
        self.expected_seq = 0
        self.missed = 0

    def on_progress(self, seq):
        gap = (seq - self.expected_seq) & 0x0FFF
        if gap:
            self.missed += gap                       # log it; the unit is still alive
        self.expected_seq = (seq + 1) & 0x0FFF
        self.last = time.monotonic()

    def alive(self):
        return (not self.enabled) or (time.monotonic() - self.last) < self.timeout
```

---

## 5. Live Logging

A **GUI checkbox** enables live logging of the serial stream, so the technician can review exactly what was sent and received.

- **Log raw words, before decoding.** Write the stream exactly as received, in the receive path, so a corrupted sweep is captured as it was, not as the decoder interpreted it.
- **Log the sweep request with the stream.** The stream is positional and can't be decoded without its plan. Each sweep's entry records the request: start/step/N (or the sub-band list), squelch, progress spacing, and the LO2 calibration table ID.
- **Log both directions.** Commands sent and words received, in order.
- **Timestamp each serial read** (not each word) with the PC's monotonic clock. That is enough to see progress timing and reference-switch stalls.
- **Make the log replayable.** The log holds both the plan and the raw words, so the same `decode_sweep()` can re-run any logged sweep offline. One decoder serves live data and review.
- **Data rate:** worst case (squelch 0, continuous sweeping) is about 2 bytes per point at the measured point rate, about 1.5 KB/s at 732 points/s.

---

## 6. Red LED

The error policy (`sa_response_protocol.md`, section 7) lights a red LED on the GUI for problems that still allow a sweep to be plotted.

- **Non-latching.** It shows the state of the trace currently displayed.
- In continuous sweeping at 30 Hz, an isolated error (a cosmic ray, say) lights it for one frame and goes unnoticed, which is intended. If the unit starts having real trouble, the LED is on more than off and becomes visible.
- In **single sweep**, or after stopping a continuous sweep, the LED describes the trace on screen and is of **paramount importance**.
- Lock failures matter mostly in single sweep or sweep-stop. Their effect on amplitude during a fast sweep is negligible, and a second single sweep generally clears them. If it doesn't, the unit may be failing, which is for the user to act on.

---

## 7. Open Items

- **Live log file format** (request record, raw word records, timestamps).
- **Optional disk cache** of Ref flags between PC program runs, keyed by CRC-32 table ID (in-session cache after `REF_FLAGS` is required; cross-run cache is optional).
