#!/usr/bin/env python
"""Compute expected MAX2871 register writes with the project C++ library."""

from __future__ import annotations

import argparse
import re
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
        printf("R5=0x%08lX R4=0x%08lX R3=0x%08lX R2=0x%08lX R1=0x%08lX R0=0x%08lX\n",
               (unsigned long)lo.Curr.Reg[5],
               (unsigned long)lo.Curr.Reg[4],
               (unsigned long)lo.Curr.Reg[3],
               (unsigned long)lo.Curr.Reg[2],
               (unsigned long)lo.Curr.Reg[1],
               (unsigned long)lo.Curr.Reg[0]);
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


def parse_harness_output(output: str) -> list[dict]:
    results = []
    for block in output.strip().split("\n\n"):
        if not block:
            continue
        result = {
            "frequency": None,
            "writes": [],
            "registers": {},
            "divider_line": None,
        }
        for line in block.splitlines():
            if line.startswith("frequency="):
                result["frequency"] = float(line.split("=", 1)[1])
                continue
            if line.startswith("M="):
                result["divider_line"] = line
                continue
            match = re.match(r"\s+\d+:\s+0x([0-9A-Fa-f]{8})$", line)
            if match:
                value = int(match.group(1), 16)
                result["writes"].append({
                    "address": value & 0x7,
                    "value": value,
                    "hex": f"0x{value:08X}",
                })
                continue
            for register, value in re.findall(r"R([0-5])=0x([0-9A-Fa-f]{8})", line):
                result["registers"][int(register)] = f"0x{int(value, 16):08X}"
        results.append(result)
    return results


def expected_registers_for_frequencies(frequencies) -> list[dict]:
    with tempfile.TemporaryDirectory() as tmp:
        harness_bin = build_harness(Path(tmp))
        completed = subprocess.run(
            [str(harness_bin), *(str(value) for value in frequencies)],
            check=True,
            capture_output=True,
            text=True,
        )
    return parse_harness_output(completed.stdout)


def expected_registers_for_frequency(frequency: float) -> dict:
    return expected_registers_for_frequencies([frequency])[0]


def main() -> int:
    args = parse_args()
    expected = expected_registers_for_frequencies(args.frequencies)
    for index, result in enumerate(expected):
        print(f"frequency={result['frequency']:.9f}")
        print(f"writes count={len(result['writes'])}")
        for write_index, write in enumerate(result["writes"]):
            print(f"  {write_index}: {write['hex']}")
        if result["divider_line"] is not None:
            print(result["divider_line"])
        registers = result["registers"]
        print(
            " ".join(
                f"R{register}=0x{int(registers[register], 16):08X}"
                for register in range(5, -1, -1)
            )
        )
        if index + 1 < len(expected):
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
