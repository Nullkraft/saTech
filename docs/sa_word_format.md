# Spectrum Analyzer Word Format and Tag Tables

This document defines the 16-bit word used on the serial link in **both directions**, and the tag tables for each direction. The protocols built on it are `sa_command_protocol.md` (PC → controller) and `sa_response_protocol.md` (controller → PC).

---

## 1. Word layout

All traffic is sent as **16-bit words**, called "packets".

```
 15  14  13  12 | 11  10   9   8   7   6   5   4   3   2   1   0
[    4-bit tag  ][              12-bit payload                   ]
```

- **Every word carries a 4-bit tag.** No word is untagged.
- **Each direction has its own tag table** (sections 3 and 4). The direction of travel tells which table applies.
- **Control words are context-free:** the tag alone says what the word is.
- **Response data words** (AMP_LOCKED, AMP_FAILLOCK, AMP_SKIPLOCK, RLE_COUNT, RLE_MULT) are **positional by nature**. The tag says what the word is, but only the decoder's cursor says _which frequency_ it belongs to. This is inherent to RLE.
- Amplitudes are raw **12-bit ADC values**. The PC converts them to dBm.

The tag is the word's upper 4 bits, so tag `0x01` puts `0x1` in bits 15–12 (an AMP_LOCKED word is `0x1nnn`).

---

## 2. Values wider than 12 bits

Any value wider than 12 bits is sent as a **zero-based pair** of words:

```
value = HI × 4096 + LO
```

- The LO word carries bits 0–11 and the HI word bits 12–23. Neither end applies an offset.
- Boundaries: 4095 → (HI 0, LO 4095). 4096 → (HI 1, LO 0).
- Each half has its own tag, so the two words can arrive in either order.

**Hardware maximum: 3,000,001 points per sweep** (3 GHz at 1 kHz steps). Every 24-bit field in both protocols is sized against this maximum. At it, the HI half of a point count or run length never exceeds 732.

---

## 3. Response tags (controller → PC)

| TagNum | Tag | Payload (12 bits) | Cursor |
| --- | --- | --- | --- |
| `0x00` | `NONE` | unused | no |
| `0x01` | `AMP_LOCKED` | ADC reading | +1 |
| `0x02` | `AMP_FAILLOCK` | ADC reading | +1 |
| `0x03` | `AMP_SKIPLOCK` | ADC reading | +1 |
| `0x04` | `RLE_COUNT` | run bits 0–11 | +value |
| `0x05` | `RLE_MULT` | run bits 12–23 | +value × 4096 |
| `0x06`–`0x07` | `SPARE` | | |
| `0x08` | `TRAILER` | segment ID | resets |
| `0x09` | `SPARE` | | |
| `0x0A` | `PTS_COUNT` | count bits 0–11 | no |
| `0x0B` | `PTS_MULT` | count bits 12–23 | no |
| `0x0C` | `ECHO` | echo field | no |
| `0x0D` | `REJECT` | reason code | no |
| `0x0E` | `SPARE` | | |
| `0x0F` | `PROGRESS` | sequence number | no |

**Meanings:**

- `NONE` (`0x00`): No message.
- `AMP_LOCKED` (`0x01`): Normal reading at or above squelch, PLL locked.
- `AMP_FAILLOCK` (`0x02`): Reading at or above squelch, taken without PLL lock. Treated exactly like AMP_LOCKED (plotted), and also logged with its frequency.
- `AMP_SKIPLOCK` (`0x03`): Reading at or above squelch, taken **without checking lock** (fast sweep). Plotted like AMP_LOCKED; not logged as a lock failure.
- `RLE_COUNT` (`0x04`): Consecutive below-squelch readings (low part of the run).
- `RLE_MULT` (`0x05`): Full 4096-point blocks of the same run. Sent only when the run is ≥ 4096.
- `SPARE` (`0x06`–`0x07`): Unassigned in the runs group (data words, which move the cursor).
- `TRAILER` (`0x08`): End of a reference segment. Opens the trailer group. Payload: segment ID, 0 = ref 1, 1 = ref 2.
- `SPARE` (`0x09`): Unassigned in the trailer group (control words, which don't move the cursor).
- `PTS_COUNT` (`0x0A`): Low part of the number of points the firmware measured in the segment.
- `PTS_MULT` (`0x0B`): Number of full 4096-point blocks (zero-based).
- `ECHO` (`0x0C`): One field of the fixed-order request echo (see `sa_command_protocol.md`, section 4).
- `REJECT` (`0x0D`): Request rejected; payload is a reason code (see `sa_command_protocol.md`, section 6).
- `SPARE` (`0x0E`): Unassigned in the status group.
- `PROGRESS` (`0x0F`): Liveness and progress. Sent every *spacing* measured points, as the PC requested (see `sa_response_protocol.md`, section 5). Payload is a 12-bit sequence number that resets at the start of each sweep.

**Codes used: 12 of 16.** Spare: `0x06`–`0x07` (runs), `0x09` (trailer), `0x0E` (status).

### 3.1 Response tag groups

The response tag values are grouped on bit boundaries, so a word's group can be read from its bits:

| Tag bits (15–12) | Range | Group |
| --- | --- | --- |
| `0000` | `0x0` | `NONE` |
| `00xx` | `0x1`–`0x3` | Amplitudes |
| `01xx` | `0x4`–`0x7` | Runs |
| `10xx` | `0x8`–`0xB` | Trailer |
| `11xx` | `0xC`–`0xF` | Status |

- **Bit 15 (tag bit 3) = 0: data word**, which moves the cursor (`NONE` aside). **Bit 15 = 1: control word**, which doesn't.
- **Bit 12 (tag bit 0) = 1 marks the MULT half of each count pair**: RLE `0x4`/`0x5` and PTS `0xA`/`0xB`. In both, the payload's weight is `value << (12 × bit0)`.
- **A hex dump reads at a glance.** The first hex digit of each word gives its group: `1`–`3` an amplitude, `4`–`5` a run, `8`/`A`/`B` the trailer, `C`/`D` echo or reject, `F` progress.

### 3.2 Run length

```
run = RLE_MULT × 4096 + RLE_COUNT
```

- Unlike the point count, the two run words are **not paired** on the PC. Each word advances the cursor as soon as it arrives (`+value` or `+value × 4096`), so their order doesn't matter and the decoder keeps no state between them.
- Runs shorter than 4096 send `RLE_COUNT` only (one word).
- Runs that are an exact multiple of 4096 send `RLE_MULT` only.
- Maximum run: 3,000,001 (the hardware maximum sweep), so `RLE_MULT` ≤ 732.

### 3.3 Point count

```
total = PTS_MULT × 4096 + PTS_COUNT
```

- Small sweeps send `PTS_MULT = 0`.
- Maximum: 3,000,001 points (the hardware maximum), so `PTS_MULT` ≤ 732.
- Example: 3,000,000 points → `PTS_MULT = 732`, `PTS_COUNT = 1728` (732 × 4096 + 1728).

---

## 4. Command tags (PC → controller)

Command pairs use **`_LO` / `_HI`** naming and assemble as in section 2 (`value = HI × 4096 + LO`). Response pairs keep `_COUNT` / `_MULT` because those words advance the decoder cursor by weighted amounts; command pairs do not.

Tag bit 0 marks the HI half of each pair (`LO` even, `HI` odd), matching the response MULT convention.

| TagNum | Tag | Payload (12 bits) | Use |
| --- | --- | --- | --- |
| `0x0` | `REQ_BEGIN` | mode bits | Opens a request. |
| `0x1` | `REQ_END` | entry count | Closes the request. |
| `0x2` | `STOP` | unused (0) | Abort continuous sweep; leave idle. |
| `0x3` | `REF_FLAGS` | unused (0) | Start Ref-flag read-back. |
| `0x4` | `SQUELCH` | ADC counts | Global squelch threshold. |
| `0x5` | `ENTRY` | entry index | Opens a sub-band entry. |
| `0x6` | `PROGRESS` | spacing in points | Global progress spacing. 0 = off; else ≤ 4095. |
| `0x7` | `SPARE` | | |
| `0x8` | `START_LO` | start kHz bits 0–11 | Entry start (pair with `START_HI`). |
| `0x9` | `START_HI` | start kHz bits 12–23 | |
| `0xA` | `STEP_LO` | step kHz bits 0–11 | Entry step (pair with `STEP_HI`). |
| `0xB` | `STEP_HI` | step kHz bits 12–23 | |
| `0xC` | `N_LO` | point count bits 0–11 | Entry point count (pair with `N_HI`). |
| `0xD` | `N_HI` | point count bits 12–23 | |
| `0xE`–`0xF` | `SPARE` | | |

### 4.1 Command tag groups

| Tag bits (15–12) | Range | Group |
| --- | --- | --- |
| `000x` | `0x0`–`0x1` | Request framing (`REQ_BEGIN` / `REQ_END`) |
| `001x` | `0x2`–`0x3` | Control (`STOP` / `REF_FLAGS`) |
| `010x` | `0x4`–`0x7` | Globals (`SQUELCH`, `ENTRY`, `PROGRESS`) |
| `10xx` / `11xx` | `0x8`–`0xD` | Entry fields (start / step / N pairs) |
| `111x` | `0xE`–`0xF` | Spare |

How these words form a request is defined in `sa_command_protocol.md`.

---

## 5. Open Items

- **Spare status `0x0E`** and command spares `0x7`, `0xE`–`0xF`.

Table ID is CRC-32 on the once-per-run `REF_FLAGS` path (`sa_command_protocol.md`, section 5), not a sweep-stream checksum.
