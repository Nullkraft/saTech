#!/usr/bin/env python
"""Interactive SCPI console for a Rigol DS1102E oscilloscope."""

import argparse
import os
import re
import readline
import select
import sys
import time


OUTPUT_PATH = "scope_dump.csv"
WAVEFORM_QUERY_RE = re.compile(
    r"^:(?:WAV|WAVEFORM):DATA\?(?:\s*(?P<source>\S+))?$",
    re.IGNORECASE,
)


def print_scpi_reply(scpi, response):
    if isinstance(response, bytes):
        if response.startswith(b"#") or waveform_buffer_name(scpi) is not None:
            payload = waveform_payload(response)
            print(f"{scpi} -> {len(response)} bytes ({len(payload)} payload bytes)")
            return

        text = response.decode("ascii", errors="replace").strip()
        if text:
            print(f"{scpi} -> {text}")
        else:
            print(f"{scpi} -> <empty>")
        return

    text = response.strip()
    if text:
        print(f"{scpi} -> {text}")
    else:
        print(f"{scpi} -> <empty>")


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
        try:
            ready, _, _ = select.select([fd], [], [], 0)
            if not ready:
                return
            if not os.read(fd, 4096):
                return
        except OSError:
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


def read_text_response(fd, delay=0.2, read_size=4096):
    time.sleep(delay)
    return os.read(fd, read_size).rstrip(b"\r\n")


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

            try:
                send_scpi(fd, command)

                buffer_name = waveform_buffer_name(command)
                if buffer_name is not None:
                    response = read_waveform_response(fd, args.query_delay, args.read_size)
                    print_scpi_reply(command, response)
                    payload = waveform_payload(response)
                    buffers[buffer_name] = normalize_payload(payload)
                    print(f"stored {len(buffers[buffer_name])} samples in {buffer_name}")
                    continue

                if stripped.endswith("?"):
                    response = read_text_response(fd, args.query_delay)
                    print_scpi_reply(command, response)
            except OSError as exc:
                print(f"{command} -> {exc}", file=sys.stderr)
                os.close(fd)
                fd = open_scope_fd(args.scope_device)
                drain_scope(fd)
    finally:
        os.close(fd)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Open the Rigol DS1102E SCPI port and provide an interactive command shell."
    )
    parser.add_argument("--scope-device", default="/dev/usbtmc0")
    parser.add_argument("--query-delay", type=float, default=0.2)
    parser.add_argument("--read-size", type=int, default=1200000)
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
