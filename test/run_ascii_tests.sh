#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
./bin/python -B -m unittest discover test -p 'ascii_*_test.py'
