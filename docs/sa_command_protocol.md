# Spectrum Analyzer Command Protocol: PC → Controller

This document defines the sweep requests the PC control/display software sends to the spectrum analyzer firmware over serial. Its companion, `sa_response_protocol.md`, defines the controller → PC direction (sweep data, trailers, progress). The word format and tag tables for both directions are in `sa_word_format.md`.

> **Status:** tag tables and request contract locked. Remaining open items are listed in section 7.

---

## 1. Overview

The PC sends **one request describing the whole sweep**, and the controller returns the RLE-encoded stream defined in `sa_response_protocol.md`. Requests are rare:

- One request per sweep configuration.
- Continuous (repeating) sweeps run from the stored request; nothing is resent per sweep.
- A multi-sub-band sweep is still one request (section 3).

Requests use the same 16-bit tagged words as the response stream, with the command tag table (`sa_word_format.md`, section 4). Every field is tagged, so a missing or garbled word is detectable instead of silently shifting the fields after it.

---

## 2. Units and ranges

**All frequencies on the controller are integer kHz.** The user enters frequencies in MHz; the PC rounds them to integer kHz before sending.

| Field | Max value | Bits needed |
| --- | --- | --- |
| Start | 3,000,000 kHz (3 GHz) | 22 |
| Step | 3,000,000 kHz | 22 |
| N (points) | 3,000,001 | 22 |
| Progress spacing | ≤ 4095 | 12 |
| Squelch | 4095 ADC counts | 12 |

Start, step, N, and table ID use **24 bits (two 12-bit payloads)**. Progress spacing and squelch fit in **one** 12-bit payload (`sa_word_format.md`, sections 2 and 4).

### Start, step and N (not stop)

The request sends the **point count N** instead of a stop frequency. Neither side has to compute the point count from (stop − start) / step, so the PC's catalog and the firmware's sweep always agree on N by construction.

The PC still shows start and stop to the user:

```
stop = start + (N − 1) × step
```

---

## 3. Request packet

Every request carries **one or more sub-band entries**. A plain single-range sweep is simply a request with one entry, so there is only one request format.

```
REQ_BEGIN(mode)
SQUELCH  PROGRESS                                       <- global settings
ENTRY(0)  START_LO START_HI  STEP_LO STEP_HI  N_LO N_HI  <- sub-band 0
ENTRY(1)  START_LO START_HI  STEP_LO STEP_HI  N_LO N_HI  <- sub-band 1
...
REQ_END(entry count)
```

| Word | Payload | Notes |
| --- | --- | --- |
| `REQ_BEGIN` | mode bits | Opens a request. Mid-sweep: abort the sweep in progress and accept the new request. |
| `SQUELCH` | ADC counts | Global. 0 = return every point. |
| `PROGRESS` | spacing in points | Global. 0 = off; otherwise ≤ 4095 and ≥ ⌈N_total / 4096⌉ (see `sa_response_protocol.md`, section 5). Full-band minimum is 733. |
| `ENTRY` | entry index (0, 1, 2, …) | Opens a sub-band entry. |
| `START_LO` / `START_HI` | start kHz, bits 0–11 / 12–23 | Belongs to the most recent `ENTRY`. |
| `STEP_LO` / `STEP_HI` | step kHz, bits 0–11 / 12–23 | Belongs to the most recent `ENTRY`. |
| `N_LO` / `N_HI` | point count, bits 0–11 / 12–23 | Belongs to the most recent `ENTRY`. |
| `REQ_END` | number of entries | Closes the request. The firmware validates it; on success it echoes and starts, on failure it sends `REJECT`. |

**Size: 4 words + 7 words per entry.** A single-range request is 11 words (22 bytes); ten sub-bands are 74 words (148 bytes).

- **Explicit begin and end.** The controller knows when it holds a complete request, and an incomplete one is discarded.
- **Entry grouping.** `START`, `STEP` and `N` words carry no entry number of their own, so they belong to the entry opened by the most recent `ENTRY` word. This is the same rule the response protocol uses for trailer fields after `TRAILER`. Within an entry, the six words are tagged and can arrive in any order.
- **Entry count check.** `REQ_END` carries the number of entries, so the firmware can confirm that none were lost, as the response protocol's point count does for points.
- **Mode bits** (in `REQ_BEGIN`):

  | Bit | Meaning |
  | --- | --- |
  | 0 | `0` = single sweep, `1` = continuous |
  | 1 | `0` = normal (check lock), `1` = fast (`AMP_SKIPLOCK`) |
  | 2–11 | reserved; must be 0 |

- **`STOP`** (command tag `0x2`) aborts a continuous sweep and leaves the controller idle. It is not a mode bit and does not open a new request.
- **Maximum entries: 32** (12 bytes of stored entry state each; headroom on the 328P's 2 KB SRAM).

### 3.1 Rules for entries

- **At least one entry.**
- **Entries are in ascending frequency order and don't overlap.** Each entry's frequencies are start, start + step, …, start + (N − 1) × step, and every frequency in the request appears exactly once.
- **Every frequency is ≤ 3,000,000 kHz** (3 GHz).
- **Total points across all entries ≤ 3,000,001**, the hardware maximum per sweep. The response protocol's limits (`PTS_MULT`, `RLE_MULT` ≤ 732, the 12-bit `PROGRESS` sequence) depend on this total.

### 3.2 Sweep order

Both sides build the **combined frequency list**: entry 0's points, then entry 1's, and so on, each entry in ascending order. The controller then:

1. Selects ref 1 and measures every ref 1 frequency, walking the combined list in order.
2. Switches references **once**.
3. Selects ref 2 and measures every ref 2 frequency, walking the combined list in order.

A sub-band sweep therefore costs two reference switches, however many entries it has, and continuous mode cycles through every sub-band. The response stream for a sub-band sweep has the same structure as for a single range (see `sa_response_protocol.md`, section 2).

---

## 4. Request echo

A corrupted request is the one error the response path can't see. The trailer's point-count check catches a disagreement about **N**, but nothing catches a disagreement about **start** or **step**. The firmware would sweep the wrong frequencies, every count check would pass, and the PC would label the trace with the frequencies it asked for. In continuous mode, every sweep would be wrong until the next request.

So the firmware **echoes what it parsed**, using response tag `ECHO` (`0x0C`):

- On a **valid** `REQ_END`, the firmware sends a fixed-order block of `ECHO` words: mode, squelch, progress spacing, **table ID lo/hi**, the entry count, then for each entry start lo/hi, step lo/hi, N lo/hi. That's **6 + 6 words per entry**.
- The PC compares the block with what it sent before trusting the sweep.
- The **table ID** is a **24-bit** value identifying the unit's calibration / Ref-flag table (see `sa_response_protocol.md`, section 3). The PC didn't send it; it compares it with the ID from the Ref-flag read-back (section 5). A mismatch means the PC would decode every sweep against the wrong Ref flags, so it must not trust the sweep. Exact ID recipe (CRC vs serial) is still open; width and echo placement are fixed.
- The block is only compared, never decoded positionally, so one tag is enough.

The echo goes out once per request. Continuous sweeps repeat from the stored request without re-echoing.

---

## 5. Ref flag read-back

The PC needs the unit's per-frequency reference assignment, its **Ref flags** (see `sa_response_protocol.md`, section 3), and **reads them back from the unit** with command tag `REF_FLAGS` (`0x3`).

- Only the flags are needed, one bit per kHz from 0 to 3,000,000 kHz: **375,001 bytes** packed, about 2 s at 2 Mbaud.
- Packed bit order: the flag for _f_ kHz is bit (_f_ mod 8), LSB first, of byte ⌊_f_ / 8⌋. Value: 0 = ref 1, 1 = ref 2.
- The read-back carries the **table ID** (24-bit, as two tagged words) before the packed bytes, so the PC can compare it with each request echo.
- The PC can cache the flags by table ID and skip the read-back when the ID hasn't changed.
- Exact framing of the byte blob after the table-ID header remains open (`sa_word_format.md`, section 5).

---

## 6. Validation and rejection

On `REQ_END` the firmware validates the assembled request.

- **Valid:** send the `ECHO` block, then start the sweep.
- **Invalid:** send one `REJECT` (`0x0D`) with a reason code in the payload; do not echo; do not start. Discard the incomplete or bad request.

| Code | Reason |
| --- | ---: |
| 1 | An entry has N = 0 |
| 2 | Bad `PROGRESS` spacing (1 … ⌈N_total/4096⌉ − 1, or > 4095) |
| 3 | A frequency above 3,000,000 kHz |
| 4 | Entries overlap or are out of ascending order |
| 5 | Total points above 3,000,001 |
| 6 | `REQ_END` entry count does not match entries received |
| 7 | More than 32 entries |
| 8 | Incomplete or malformed request |

---

## 7. Open Items

- **Table ID recipe:** CRC of the Ref-flag / calibration table vs a calibration serial number (24-bit width is fixed).
- **`REF_FLAGS` byte-stream framing** after the table-ID header.
