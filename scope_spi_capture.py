#!/usr/bin/env python
"""Capture a Rigol DS1102E RAW waveform after cycling saTech LO1."""

import argparse
import os
import sys
import time

import serial


LO1_FMN_600 = "BA0FFC48"
LO1_FMN_500 = "9B0FFC3C"
LO1_FMN_SELECT = "31FF"


def write_scope(fd, scpi, delay=0.05):
    os.write(fd, scpi.encode("ascii") + b"\n")
    time.sleep(delay)


def query_scope(fd, scpi, read_size, delay=0.15):
    os.write(fd, scpi.encode("ascii") + b"\n")
    time.sleep(delay)
    return os.read(fd, read_size).decode("ascii", errors="replace").strip()


def read_scope_bytes(fd, scpi, count, delay=0.15):
    os.write(fd, scpi.encode("ascii") + b"\n")
    time.sleep(delay)

    chunks = []
    received = 0
    while received < count:
        chunk = os.read(fd, min(4096, count - received))
        if not chunk:
            break
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def wait_for_scope_status(fd, wanted, timeout_s, poll_s):
    deadline = time.monotonic() + timeout_s
    last = ""
    while time.monotonic() < deadline:
        last = query_scope(fd, ":TRIGger:STATus?", 64)
        if last == wanted:
            return last
        time.sleep(poll_s)
    raise TimeoutError(f"scope status stayed {last!r}; expected {wanted!r}")


def setup_scope_for_normal_wait(fd, args):
    write_scope(fd, ":CHAN1:DISP ON")
    write_scope(fd, ":CHAN2:DISP ON")
    write_scope(fd, ":TRIGger:MODE EDGE")
    write_scope(fd, ":TRIGger:EDGE:SWEep NORM")
    write_scope(fd, ":WAVeform:POINts:MODE RAW")
    write_scope(fd, ":RUN")
    return wait_for_scope_status(fd, "WAIT", args.wait_timeout, args.poll_interval)


def cycle_lo1(args):
    commands = [LO1_FMN_SELECT, LO1_FMN_600, LO1_FMN_500]
    with serial.Serial(args.serial_port, args.baud, timeout=0.05) as ser:
        time.sleep(args.serial_open_delay)
        ser.reset_input_buffer()
        for command in commands:
            ser.write((command + "\n").encode("ascii"))
            ser.flush()
            time.sleep(args.serial_command_delay)


def write_csv(path, ch1, ch2):
    if len(ch1) != len(ch2):
        raise ValueError(f"channel length mismatch: ch1={len(ch1)} ch2={len(ch2)}")

    with open(path, "w", encoding="ascii", newline="\n") as out:
        out.write("sample,ch1_raw,ch2_raw\n")
        for index, (ch1_byte, ch2_byte) in enumerate(zip(ch1, ch2)):
            out.write(f"{index},{ch1_byte},{ch2_byte}\n")


def capture_scope(args):
    fd = os.open(args.scope_device, os.O_RDWR)
    try:
        setup_scope_for_normal_wait(fd, args)
        cycle_lo1(args)
        wait_for_scope_status(fd, "T'D", args.trigger_timeout, args.poll_interval)
        write_scope(fd, ":STOP")
        wait_for_scope_status(fd, "STOP", args.wait_timeout, args.poll_interval)
        write_scope(fd, ":WAVeform:POINts:MODE RAW")
        ch1 = read_scope_bytes(fd, ":WAVeform:DATA? CHAN1", args.points)
        ch2 = read_scope_bytes(fd, ":WAVeform:DATA? CHAN2", args.points)
    finally:
        os.close(fd)

    if len(ch1) != args.points or len(ch2) != args.points:
        raise RuntimeError(
            f"capture length mismatch: ch1={len(ch1)} ch2={len(ch2)} "
            f"expected={args.points}"
        )

    write_csv(args.output, ch1, ch2)
    print(f"captured {len(ch1)} samples per channel to {args.output}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Set Rigol DS1102E to normal/wait, cycle LO1 600->500, stop, and dump RAW waveform data."
    )
    parser.add_argument("--scope-device", default="/dev/usbtmc0")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", default="scope_dump.csv")
    parser.add_argument("--points", type=int, default=8192)
    parser.add_argument("--wait-timeout", type=float, default=5.0)
    parser.add_argument("--trigger-timeout", type=float, default=5.0)
    parser.add_argument("--poll-interval", type=float, default=0.05)
    parser.add_argument("--serial-open-delay", type=float, default=3.0)
    parser.add_argument("--serial-command-delay", type=float, default=0.2)
    return parser.parse_args()


def main():
    try:
        capture_scope(parse_args())
    except Exception as exc:
        print(f"scope_lo1_capture: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
