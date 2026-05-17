#!/usr/bin/env python
"""Interactive SCPI console for a Rigol DS1102E oscilloscope."""

import argparse
import os
import re
import readline
import select
import sys
import time


LO1_FMN_600 = "BA0FFC48"
LO1_FMN_500 = "9B0FFC3C"
LO1_FMN_SELECT = "31FF"
OUTPUT_PATH = "scope_dump.csv"
WAVEFORM_QUERY_RE = re.compile(
    r"^:(?:WAV|WAVEFORM):DATA\?(?:\s*(?P<source>\S+))?$",
    re.IGNORECASE,
)


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
    import serial

    commands = [LO1_FMN_SELECT, LO1_FMN_600, LO1_FMN_500]
    with serial.Serial(args.serial_port, args.baud, timeout=0.05) as ser:
        time.sleep(args.serial_open_delay)
        ser.reset_input_buffer()
        for command in commands:
            ser.write((command + "\n").encode("ascii"))
            ser.flush()
            big_number = 0
            # while big_number < 1000:
            #     big_number += 1
            # time.sleep(args.serial_command_delay)


def write_csv(path, ch1, ch2):
    with open(path, "w", encoding="ascii", newline="\n") as out:
        out.write("sample,ch1,ch2\n")
        sample_count = max(len(ch1), len(ch2))
        for index in range(sample_count):
            ch1_value = "" if index >= len(ch1) else ch1[index]
            ch2_value = "" if index >= len(ch2) else ch2[index]
            out.write(f"{index},{ch1_value},{ch2_value}\n")


def open_scope_fd(path):
    return os.open(path, os.O_RDWR | getattr(os, "O_CLOEXEC", 0))


def drain_scope(fd):
    while True:
        ready, _, _ = select.select([fd], [], [], 0)
        if not ready:
            return
        if not os.read(fd, 4096):
            return


def send_scpi(fd, scpi):
    os.write(fd, scpi.encode("ascii") + b"\n")


def read_scpi_response(fd, timeout=1.0):
    deadline = time.monotonic() + timeout
    data = bytearray()
    block_len = None

    while True:
        if block_len is not None and len(data) >= block_len:
            break

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break

        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            break

        chunk = os.read(fd, 4096)
        if not chunk:
            break

        data.extend(chunk)
        deadline = time.monotonic() + timeout

        if block_len is None and len(data) >= 2 and data[0] == ord("#"):
            digit_count = data[1] - ord("0")
            if 0 <= digit_count <= 9 and len(data) >= 2 + digit_count:
                if digit_count > 0:
                    payload_len = int(data[2 : 2 + digit_count].decode("ascii"))
                    block_len = 2 + digit_count + payload_len
                else:
                    block_len = None

        if block_len is None and (data.endswith(b"\n") or data.endswith(b"\r")):
            ready, _, _ = select.select([fd], [], [], 0)
            if not ready:
                break

    return bytes(data)


def read_waveform_response(fd, delay=0.2, read_size=1200000):
    time.sleep(delay)
    return os.read(fd, read_size).rstrip(b"\n")


def waveform_buffer_name(command):
    match = WAVEFORM_QUERY_RE.match(command.strip())
    if not match:
        return None

    source = match.group("source")
    if not source:
        return None

    source = source.upper().replace("CHANNEL", "CHAN")
    if source == "CHAN1":
        return "ch1"
    if source == "CHAN2":
        return "ch2"
    return None


def waveform_payload(response):
    if len(response) < 2 or response[0:1] != b"#":
        return response

    digit_count = response[1] - ord("0")
    if not 0 <= digit_count <= 9:
        return response

    if digit_count == 0:
        return response[2:].rstrip(b"\r\n")

    header_end = 2 + digit_count
    if len(response) < header_end:
        return response

    payload_len = int(response[2:header_end].decode("ascii"))
    payload_end = header_end + payload_len
    if len(response) < payload_end:
        return response

    return response[header_end:payload_end]


def complement_payload(payload):
    return [byte ^ 0xFF for byte in payload]


def normalize_payload(payload):
    values = complement_payload(payload)
    if not values:
        return values

    minimum = min(values)
    return [value - minimum for value in values]


def run_console(args):
    fd = open_scope_fd(args.scope_device)
    buffers = {"ch1": [], "ch2": []}

    try:
        drain_scope(fd)
        while True:
            try:
                command = input("scpi> ")
            except EOFError:
                break
            except KeyboardInterrupt:
                print()
                break

            stripped = command.strip()
            if not stripped:
                continue

            if hasattr(readline, "add_history"):
                readline.add_history(command)

            if stripped.lower() == "exit":
                break

            if stripped.lower() == "save":
                write_csv(OUTPUT_PATH, buffers["ch1"], buffers["ch2"])
                print(f"saved {OUTPUT_PATH}")
                continue

            send_scpi(fd, command)

            buffer_name = waveform_buffer_name(command)
            if buffer_name is not None:
                response = read_waveform_response(fd, args.query_delay, args.read_size)
                payload = waveform_payload(response)
                buffers[buffer_name] = normalize_payload(payload)
                print(f"stored {len(buffers[buffer_name])} samples in {buffer_name}")
                continue

            if stripped.endswith("?"):
                response = read_scpi_response(fd)
                text = response.decode("ascii", errors="replace").strip()
                if text:
                    print(text)
    finally:
        os.close(fd)


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
        description="Open the Rigol DS1102E SCPI port and provide an interactive command shell."
    )
    parser.add_argument("--scope-device", default="/dev/usbtmc0")
    parser.add_argument("--serial-port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", default="scope_dump.csv")
    parser.add_argument("--points", type=int, default=8192)
    parser.add_argument("--query-delay", type=float, default=0.2)
    parser.add_argument("--read-size", type=int, default=1200000)
    parser.add_argument("--wait-timeout", type=float, default=5.0)
    parser.add_argument("--trigger-timeout", type=float, default=5.0)
    parser.add_argument("--poll-interval", type=float, default=0.05)
    parser.add_argument("--serial-open-delay", type=float, default=3.0)
    parser.add_argument("--serial-command-delay", type=float, default=0.2)
    return parser.parse_args()


def main():
    try:
        run_console(parse_args())
    except Exception as exc:
        print(f"scope_scpi_console: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
