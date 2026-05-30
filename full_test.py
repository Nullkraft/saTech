#!/usr/bin/env python
"""Command-line entry point for the saTech full test."""

import argparse
import json

from rigol_full_test_adapter import RigolFullTestAdapter
from run_full_test import DEFAULT_EXPECTED_ID, FullTestConfig, run_full_test


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
    report = run_full_test(config, rigol=RigolFullTestAdapter())
    print(json.dumps(report.to_dict(), indent=2))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
