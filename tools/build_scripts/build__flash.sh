#!/bin/bash
if [ $# -eq 0 ]; then
    echo "Usage: $0 <pio run arguments>"
    echo "Example: $0 -e technician -t upload --upload-port /dev/ttyUSB0"
    exit 1
fi

rm -rf .pio/libdeps/*
pio run -t clean && pio run "$@"
