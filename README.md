# saTech

Firmware and support code for bringing up Gen 4 SpecAnn circuit boards.

## Command model

PC ASCII serial input is raw hex word transport only. A line such as `0CFF` is parsed into the same 32-bit command word used by binary serial input and then routed through the shared binary control path.

Named technician commands, such as `help`, `status`, `set`, `chip`, `atten`, and `spi`, live in `src/technician_console.cpp` and `src/technician_console.h`.

## Loop selection

`src/main_entry.cpp` uses the compile-time `SATECH_TECHNICIAN_CONSOLE` switch:

- `0` (default): `loop()` calls `pollSerial()`.
- `1`: `loop()` calls `pollTechnicianConsole()` and `setup()` prints the technician banner.

This keeps the PC transport and the technician console separate entry points while preserving one shared binary command execution path.
