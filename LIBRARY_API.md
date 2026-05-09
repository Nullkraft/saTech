# Library API Checklist

## Public Setup API

- [x] Provide `saTech.begin(...)` for `ascii` and `binary`, accepting
      capitalization variants for the encoding string.
- [x] Ignore unsupported encoding strings.
- [x] Keep encoding selection outside the runtime payload state machine.
- [x] Allow sketches to hard-code the encoding in `setup()`.
- [x] Allow sketches to wait for a serial token of `ascii` or `binary` before
      calling `saTech.begin(...)`.
- [x] If waiting for the token, periodically print a reminder and continue
      waiting rather than falling through to runtime behavior.

## Runtime Receiver Model

- [x] Track serial transport encoding separately from payload mode.
- [x] Use an encoding enum equivalent to ASCII versus binary.
- [x] Use a payload mode enum equivalent to command, FMN data, and direct
      register data.
- [x] In binary encoding, collect a complete 4-byte word before processing.
- [x] In ASCII encoding, receive a complete token or line, convert it to the
      same internal `uint32_t` word format, then process it.
- [x] Keep ASCII parsing and binary parsing in codec-level functions.
- [x] Route both codecs into one shared `processReceivedWord(uint32_t word)`
      style function.
- [x] Do not mix ASCII-vs-binary detection into normal payload processing.

## Word Formats

- [x] Define wire byte order explicitly as little-endian for 32-bit words:
      the first received byte is bits `[7 downto 0]`, the second byte is
      bits `[15 downto 8]`, the third byte is bits `[23 downto 16]`, and the
      fourth byte is bits `[31 downto 24]`.
- [x] FMN packed data:
      `F=[31 downto 20]`, `M=[19 downto 8]`, `N=[7 downto 0]`.
- [x] Document that FMN `N` will never equal `0xFF`; the low byte value
      `0xFF` is reserved for command words.
- [x] Direct register data:
      32-bit register word with `Addr=[2 downto 0]`.
- [x] Document that direct register data LS byte will never equal `0xFF`; the
      low byte value `0xFF` is reserved for command words.
- [x] Command and control data:
      `data=[31 downto 16]`, `command=[15 downto 11]`,
      `address=[10 downto 8]`, `command_flag=[7 downto 0]`.
- [x] Document that `command_flag` is always `0xFF`.
- [x] Account for command `data` bits even while currently unused.
- [x] Treat command `address` as the selected Arduino chip-select pin target.

## Payload State Machine

- [x] Start runtime payload mode in command mode.
- [x] In any payload mode, classify a completed word with LS byte `0xFF` as a
      command word.
- [x] In FMN data mode, classify completed non-command words as FMN data.
- [x] In direct register mode, classify completed non-command words as direct
      register data.
- [x] Enter command mode on command word `0x000006FF`.
- [x] Enter FMN data mode on command word `0x00000EFF`.
- [x] Enter direct register mode on command word `0x000016FF`.
- [x] From direct register mode, allow switching only through a command word.
- [x] From FMN data mode, allow switching only through a command word.
- [x] From command mode, allow switching to FMN data or direct register mode.
- [x] From command mode, allow switching between ASCII and binary transport
      encoding.

## Binary Receive Details

- [x] Use a small partial buffer or equivalent logic to collect exactly 4 bytes.
- [x] Do not process a binary word until all 4 bytes have been received.
- [x] Treat short reads/timeouts as receive errors or incomplete words, not as
      partially valid commands.
- [x] Preserve the rule that command words are recognized at 32-bit word
      boundaries, not by scanning arbitrary byte positions.

## ASCII Receive Details

- [x] Receive the full ASCII token or line before conversion.
- [x] Convert ASCII command tokens to the same binary command words used by
      binary encoding.
- [x] Convert ASCII FMN/register payloads to the same `uint32_t` words used by
      binary encoding.
- [x] Keep all downstream command, FMN, and register handling shared after
      conversion.
