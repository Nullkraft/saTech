#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
./bin/python serial_command_test.py --port "${1:-${SATECH_PORT:-/dev/ttyUSB2}}" select-adc2
