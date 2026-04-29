# Serial Communication Plan

## Design Principle

The firmware accepts two serial encodings:

1. Binary: fixed 4-byte little-endian words for host automation.
2. ASCII: newline-terminated technician text for manual use.

Both encodings may dispatch into the same internal command handlers, but they
do not need identical user-facing behavior. Binary should stay terse and
deterministic. ASCII may be friendlier, more verbose, and safer for technician
bring-up.

The protocol must stay host-language independent. The host may be Python,
FreeBASIC, a terminal, or another program; the firmware contract is the serial
byte stream, not the host implementation language.

## Serial Receive Modes

```cpp
enum class SerialRxMode {
    AsciiLineData,
    BinaryControlWord,
    FMNData,
    Direct2Register,
};
```

`SerialRxMode` describes how incoming bytes are decoded right now. It should be
separate from the selected hardware target.

The receiver should also track the current binary target, such as LO1 or LO2,
so that decoded data words can be routed without overloading the receive mode.

## Word Types

### ControlWord

A control word is a 4-byte little-endian word whose least significant byte is
`0xFF`. In the serial stream, that means the first received byte is `0xFF`.

Control words select modes, targets, references, and simple control actions.
Examples:

- `0x17FF`: report the Arduino message.
- `0x0FFF`: turn the Arduino LED on.
- `0x07FF`: turn the Arduino LED off.
- `0x0CFF`: select reference clock 1.
- `0x14FF`: select reference clock 2.
- `0x04FF`: disable reference clocks.
- `0x01FF`: select LO1.
- `0x02FF`: select LO2.
- `0x20FF`: enter AsciiLineData mode. Proposed; not yet approved.
- `0x40FF`: enter FMNData mode. Proposed; not yet approved.
- `0x80FF`: enter Direct2Register mode. Proposed; not yet approved.

LO3 control may be added later, but is intentionally out of scope for the first
bring-up slice.

### FMNData

An FMN data word is a 4-byte packed MAX2871 payload. The least significant byte
contains the 8-bit N value. Valid N values for the intended use never require
`0xFF`, so an incoming `0xFF` at a frame boundary may be treated as the start of
a new control word.

FMN data is initially needed for LO2 programming. LO3 FMN programming is out of
scope for the first bring-up slice.

### Direct2Register

A direct-register word is an unmodified 32-bit value sent straight to the
selected chip register path. MAX2871 register addresses only use register
numbers 0 through 6, so direct-register words should not have `0xFF` as the
least significant byte. That leaves `0xFF` available as the control-word marker
at a frame boundary.

## ASCII Handling

ASCII technician input is newline-terminated text. Named technician commands may
continue to exist as technician-only commands.

ASCII may also spell a control word as hex text. For example:

```text
17FF
```

That text is transmitted as printable ASCII bytes, then parsed into the same
internal value as the binary control word:

```text
ASCII bytes: 31 37 46 46 0A
value:       0x000017FF
binary:      FF 17 00 00
```

This avoids a byte-level conflict because ASCII `F` is `0x46`; it is not the
binary byte `0xFF`.

## Host-Side Serial Test Tool

Add a small host-side serial command test tool before coupling this bring-up
work to WN2A_RFPeakSearch. The tool should send individual commands directly to
the Arduino and print exactly what happened:

```text
TX bytes
RX bytes
RX printable text, when applicable
```

Example command names:

- `arduino-message`: send `FF 17 00 00`, expect `saTech WN2A ready`.
- `ascii-message`: send `17FF\n`, expect `saTech WN2A ready`.
- `enter-ascii`: send `FF 20 00 00`.
- `enter-fmn`: send `FF 40 00 00`.
- `enter-direct`: send `FF 80 00 00`.
- `select-lo1`: send `FF 01 00 00`.
- `select-lo2`: send `FF 02 00 00`.
- `select-ref1`: send `FF 0C 00 00`.
- `select-ref2`: send `FF 14 00 00`.
- `ref-off`: send `FF 04 00 00`.

This tool is for protocol bring-up and repeatable debugging. WN2A_RFPeakSearch
should be updated after the firmware protocol stabilizes, not for every
individual firmware step.

## State Machine Sketch

At a frame boundary, the receiver is ready for the first byte of the next
4-byte word. If that first byte is `0xFF`, the word is decoded as a
BinaryControlWord. Bytes inside a partially collected 4-byte word are data and
are not interpreted as mode markers.

Mode changes should be explicit control words. For example, target selection
and receive-mode selection can be separate commands:

```text
FF 02 00 00   -> ControlWord 0x02FF, select LO2
FF 40 00 00   -> ControlWord 0x40FF, enter FMNData
<4 FMN bytes> -> program LO2
<4 FMN bytes> -> program LO2 again
FF 20 00 00   -> ControlWord 0x20FF, return to AsciiLineData
```

```text
AsciiLineData:
  printable bytes and line endings -> ASCII line decoder
  byte 0xFF at frame boundary      -> collect BinaryControlWord

BinaryControlWord:
  collect 4 bytes total
  decode selector/control command
  perform action, select target, or change receive mode

FMNData:
  byte 0xFF at frame boundary -> collect BinaryControlWord
  otherwise collect 4 bytes   -> program selected LO from packed FMN data

Direct2Register:
  byte 0xFF at frame boundary -> collect BinaryControlWord
  otherwise collect 4 bytes   -> write raw register word to selected target
```

## Implementation Checklist

- [ ] Add a small host-side serial command test tool.
- [ ] Add named tests for the current Arduino message binary command.
- [ ] Add `SerialRxMode` with `AsciiLineData`, `BinaryControlWord`,
      `FMNData`, and `Direct2Register`.
- [ ] Add serial receive state near `pollSerial()`: current mode, selected
      binary target, 4-byte buffer, and byte count.
- [ ] Keep receive mode separate from selected binary hardware target.
- [ ] Replace the current non-printable-byte binary heuristic with frame-boundary
      handling for `0xFF`.
- [ ] Route binary 4-byte control frames into a shared `handleControlWord()`.
- [ ] Add explicit mode selection control words for `AsciiLineData`, `FMNData`,
      and `Direct2Register`.
- [ ] Allow ASCII hex control words to dispatch into `handleControlWord()`.
- [ ] Keep existing named ASCII technician commands working.
- [ ] Keep binary control responses quiet except for explicit query responses.
- [ ] Add quiet reference selection for WN2A control words.
- [ ] Add LO1 and LO2 target selection control words.
- [ ] Add LO1 programming from WN2A control words.
- [ ] Add LO2 programming from FMN data words.
- [ ] Add Direct2Register mode after the minimum LO bring-up path is working.
- [ ] Exclude LO3 target selection and LO3 FMN programming from the first slice.
- [ ] Add named tests for individual ASCII and binary serial commands.
- [ ] Build with `pio run -e ci`.
- [ ] Verify Arduino message query over binary.
- [ ] Verify ASCII `17FF` reaches the same internal control action as binary
      `FF 17 00 00`.
