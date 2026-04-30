#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
pio test -e uno -f test_ref_pin_readback
