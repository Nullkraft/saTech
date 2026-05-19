#!/usr/bin/env python
"""Compute expected MAX2871 register writes with the project C++ library."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
MAX2871_SRC = REPO_ROOT / ".pio/libdeps/native/MAX2871_PLL/src"


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "max2871.h"
#include "max2871_transport.h"
#include "mcu_hal.h"

class CaptureHAL : public I_MAX2871Transport, public IDelayProvider {
public:
    std::vector<uint32_t> writes;
    void delayMs(uint32_t) override {}
    void spiWriteRegister(uint32_t value) override { writes.push_back(value); }
    bool readMuxout() override { return false; }
};

static void print_writes(const char* label, const std::vector<uint32_t>& writes) {
    printf("%s count=%zu\n", label, writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
        printf("  %zu: 0x%08lX\n", i, (unsigned long)writes[i]);
    }
}

int main(int argc, char** argv) {
    CaptureHAL hal;
    MAX2871 lo(66.0, hal, hal);
    lo.begin();
    lo.outputSelect(RF_B);
    lo.outputPower(5, RF_B);
    hal.writes.clear();

    for (int i = 1; i < argc; ++i) {
        const double mhz = atof(argv[i]);
        lo.setFrequency(mhz);

        printf("frequency=%.9f\n", mhz);
        print_writes("writes", hal.writes);
        printf("M=%u Frac=%lu N=%u DIVA=%u actual=%.9f\n",
               lo.M, (unsigned long)lo.Frac, lo.N, lo.DIVA, lo.fmn2freq());
        printf("R0=0x%08lX R1=0x%08lX R4=0x%08lX\n",
               (unsigned long)lo.Curr.Reg[0],
               (unsigned long)lo.Curr.Reg[1],
               (unsigned long)lo.Curr.Reg[4]);
        if (i + 1 < argc) {
            printf("\n");
        }

        hal.writes.clear();
    }
    return 0;
}
'''


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print expected MAX2871 writes for one or more LO frequencies in MHz."
    )
    parser.add_argument("frequencies", nargs="+", type=float)
    return parser.parse_args()


def build_harness(tmpdir: Path) -> Path:
    harness_cpp = tmpdir / "max2871_expected.cpp"
    harness_bin = tmpdir / "max2871_expected"
    harness_cpp.write_text(HARNESS, encoding="ascii")

    subprocess.run(
        [
            "g++",
            "-std=c++11",
            f"-I{MAX2871_SRC}",
            str(harness_cpp),
            str(MAX2871_SRC / "max2871.cpp"),
            "-o",
            str(harness_bin),
        ],
        check=True,
    )
    return harness_bin


def main() -> int:
    args = parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        harness_bin = build_harness(Path(tmp))
        subprocess.run([str(harness_bin), *(str(value) for value in args.frequencies)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
