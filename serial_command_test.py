#!/usr/bin/env python
"""Small serial command test tool for saTech bring-up."""

import argparse
import string
import sys
import time

import serial


COMMANDS = {
    "arduino-message": {
        "tx": bytes.fromhex("FF 17 00 00"),
        "expect": "saTech WN2A ready",
    },
    "ascii-message": {
        "tx": b"17FF\n",
        "expect": "saTech WN2A ready",
    },
    "ascii-enter-ascii": {"tx": b"06FF\n"},
    "ascii-enter-fmn": {"tx": b"0EFF\n"},
    "ascii-enter-direct": {"tx": b"16FF\n"},
    "ascii-select-ref1": {"tx": b"0CFF\n"},
    "ascii-select-ref2": {"tx": b"14FF\n"},
    "ascii-ref-off": {"tx": b"04FF\n"},
    "enter-ascii": {"tx": bytes.fromhex("FF 06 00 00")},
    "enter-fmn": {"tx": bytes.fromhex("FF 0E 00 00")},
    "enter-direct": {"tx": bytes.fromhex("FF 16 00 00")},
    "select-atten": {"tx": bytes.fromhex("FF 08 7F 00")},
    "select-lo1": {"tx": bytes.fromhex("FF 01 00 00")},
    "select-lo2": {"tx": bytes.fromhex("FF 02 00 00")},
    "select-lo3": {"tx": bytes.fromhex("FF 03 00 00")},
    "select-adc1": {"tx": bytes.fromhex("FF 05 00 00")},
    "select-adc2": {"tx": bytes.fromhex("FF 0D 00 00")},
    "select-ram": {"tx": bytes.fromhex("FF 15 00 00")},
    "select-flash": {"tx": bytes.fromhex("FF 1D 00 00")},
    "select-ref1": {"tx": bytes.fromhex("FF 0C 00 00")},
    "select-ref2": {"tx": bytes.fromhex("FF 14 00 00")},
    "ref-off": {"tx": bytes.fromhex("FF 04 00 00")},
}


def hex_bytes(data):
    return " ".join(f"{byte:02X}" for byte in data)


def printable_text(data):
    text = data.decode("utf-8", errors="replace").strip()
    if not text:
        return ""
    printable = set(string.printable)
    return "".join(ch if ch in printable else "." for ch in text)


def read_response(ser, timeout):
    deadline = time.monotonic() + timeout
    chunks = []

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        if waiting:
            chunks.append(ser.read(waiting))
            deadline = time.monotonic() + 0.1
            continue
        time.sleep(0.01)

    return b"".join(chunks)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Send one named saTech serial command and print TX/RX bytes."
    )
    parser.add_argument("--port", required=True, help="Serial device path")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        help="Seconds to wait for response bytes",
    )
    parser.add_argument(
        "--open-delay",
        type=float,
        default=3.0,
        help="Seconds to wait after opening the serial port",
    )
    parser.add_argument("command", choices=sorted(COMMANDS))
    return parser.parse_args()


def main():
    args = parse_args()
    command = COMMANDS[args.command]
    tx = command["tx"]

    print(f"TX bytes: {hex_bytes(tx)}")

    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        time.sleep(args.open_delay)
        ser.reset_input_buffer()
        ser.write(tx)
        ser.flush()
        rx = read_response(ser, args.timeout)

    print(f"RX bytes: {hex_bytes(rx) if rx else '(none)'}")

    text = printable_text(rx)
    if text:
        print(f"RX text: {text}")

    expected = command.get("expect")
    if expected and expected not in text:
        print(f"Expected text not found: {expected}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
