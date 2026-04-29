#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
pio test -e uno -f test_ref_off_pin_readback
