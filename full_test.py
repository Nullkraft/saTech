#!/usr/bin/env python
"""Command-line entry point for the saTech full test."""

import argparse
import glob
import importlib.util
import json
from pathlib import Path

import serial
from rigol_full_test_adapter import RigolFullTestAdapter
from run_full_test import DEFAULT_EXPECTED_ID, FullTestConfig, run_full_test


BK390A_PROJECT = Path("~/projects/MCP/BK-Precision-390A-MCP").expanduser()
BK390A_DEFAULT_PORT = "/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller_D-if00-port0"
BK390A_PORT_MARKERS = (
    "usb-Prolific_Technology_Inc._USB-Serial_Controller",
    "Prolific",
)
BK390A_GLOB_PATTERNS = (
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
    "/dev/serial/by-id/*",
)


def _load_bk390a_parser():
    parser_path = BK390A_PROJECT / "bk390a_parser.py"
    spec = importlib.util.spec_from_file_location("bk390a_parser", parser_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load BK390A parser from {parser_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BK390A_PARSER = _load_bk390a_parser()


class Bk390aFullTestAdapter:
    def __init__(self, port=BK390A_DEFAULT_PORT, timeout_s=2.0, require_stable=True, max_frames=6):
        self.port = port
        self.timeout_s = timeout_s
        self.require_stable = require_stable
        self.max_frames = max_frames

    def read_measurement(self):
        port = self._resolve_port(self.port)
        with serial.Serial(
            port=port,
            baudrate=2400,
            bytesize=serial.SEVENBITS,
            parity=serial.PARITY_ODD,
            stopbits=serial.STOPBITS_ONE,
            timeout=self.timeout_s,
        ) as handle:
            raw_frame = self._read_frame(handle)
            measurement = BK390A_PARSER.parse_frame(raw_frame)
            frames_seen = 1

            if not self.require_stable:
                return {
                    "port": port,
                    "stable": False,
                    "frames_seen": frames_seen,
                    "raw_frame": raw_frame,
                    "measurement": measurement,
                }

            previous_raw = raw_frame
            previous_measurement = measurement
            for _ in range(1, self.max_frames):
                next_raw = self._read_frame(handle)
                next_measurement = BK390A_PARSER.parse_frame(next_raw)
                frames_seen += 1
                if next_raw == previous_raw:
                    return {
                        "port": port,
                        "stable": True,
                        "frames_seen": frames_seen,
                        "raw_frame": next_raw,
                        "measurement": next_measurement,
                    }
                previous_raw = next_raw
                previous_measurement = next_measurement

            return {
                "port": port,
                "stable": False,
                "frames_seen": frames_seen,
                "raw_frame": previous_raw,
                "measurement": previous_measurement,
            }

    def _read_frame(self, handle):
        while True:
            raw = handle.readline()
            if not raw:
                raise TimeoutError(f"timed out waiting for data from {handle.port}")
            text = raw.decode("ascii", errors="strict").strip()
            if text:
                return text

    @staticmethod
    def _resolve_port(port):
        if port != BK390A_DEFAULT_PORT:
            return port
        for marker in BK390A_PORT_MARKERS:
            for candidate in Bk390aFullTestAdapter._list_candidate_ports():
                if marker in candidate:
                    return candidate
        return port

    @staticmethod
    def _list_candidate_ports():
        ports = []
        for pattern in BK390A_GLOB_PATTERNS:
            ports.extend(glob.glob(pattern))
        return sorted(set(ports))


def parse_args():
    parser = argparse.ArgumentParser(description="Run the saTech full-test workflow.")
    parser.add_argument("--port", required=True, help="Technician console serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        help="Seconds to wait for each command response",
    )
    parser.add_argument(
        "--open-delay",
        type=float,
        default=3.0,
        help="Seconds to wait after opening the serial port",
    )
    parser.add_argument(
        "--expected-id",
        default=DEFAULT_EXPECTED_ID,
        help="Expected unit identification text",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    config = FullTestConfig(
        port=args.port,
        baud=args.baud,
        timeout=args.timeout,
        open_delay=args.open_delay,
        expected_id=args.expected_id,
    )
    report = run_full_test(
        config,
        rigol=RigolFullTestAdapter(),
        meter=Bk390aFullTestAdapter(),
    )
    print(json.dumps(report.to_dict(), indent=2))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
