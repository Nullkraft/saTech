#!/usr/bin/env python
"""Decode SPI data samples captured from the Rigol scope."""

import argparse
import csv
import json
import re
import sys


class DecodeError(Exception):
    pass


def parse_sample_indexes(text):
    text = text.strip()
    if not text:
        return []

    if text.startswith("["):
        return [int(value) for value in json.loads(text)]

    return [int(value) for value in re.split(r"[\s,]+", text) if value]


def load_sample_indexes(path):
    with open(path, "r", encoding="ascii") as infile:
        return parse_sample_indexes(infile.read())


def load_channel_from_csv(path, channel):
    samples = []
    with open(path, "r", encoding="ascii", newline="") as infile:
        reader = csv.DictReader(infile)
        for row in reader:
            value = row[channel].strip()
            samples.append(None if value == "" else int(value))
    return samples


def sample_to_bit(sample, ch2_max, low_ratio=0.2, high_ratio=0.8):
    if sample > (high_ratio * ch2_max):
        return 0x1
    if sample < (low_ratio * ch2_max):
        return 0x0
    raise DecodeError(
        f"sample value {sample} is between low/high thresholds for ch2_max={ch2_max}"
    )


def decode_data_words(data_values, sample_indexes):
    if len(sample_indexes) % 32 != 0:
        raise DecodeError(
            f"sample_indexes count {len(sample_indexes)} is not divisible by 32"
        )

    valid_samples = [sample for sample in data_values if sample is not None]
    if not valid_samples:
        raise DecodeError("channel 2 has no data samples")

    ch2_max = max(valid_samples)
    decoded_words = []
    address_map = {}

    for group_start in range(0, len(sample_indexes), 32):
        data_word = 0
        group_indexes = sample_indexes[group_start : group_start + 32]

        for sample_index in group_indexes:
            if sample_index < 0 or sample_index >= len(data_values):
                raise DecodeError(f"sample index {sample_index} is outside channel 2")

            sample = data_values[sample_index]
            if sample is None:
                raise DecodeError(f"sample index {sample_index} has no channel 2 value")

            bit = sample_to_bit(sample, ch2_max)
            data_word = (data_word << 1) | bit

        address = data_word & 0x7
        decoded_word = {
            "word_index": len(decoded_words),
            "address": address,
            "value": data_word,
            "hex": f"0x{data_word:08X}",
        }
        decoded_words.append(decoded_word)
        address_map[address] = decoded_word

    return {
        "ch2_max": ch2_max,
        "sample_index_count": len(sample_indexes),
        "decoded_words": decoded_words,
        "address_map": {
            str(address): (address_map[address]["hex"] if address in address_map else None)
            for address in range(8)
        },
    }


def parse_args():
    parser = argparse.ArgumentParser(
        description="Decode normalized channel 2 SPI samples at rising-edge sample indexes."
    )
    parser.add_argument("--csv", default="scope_dump.csv")
    parser.add_argument("--channel", default="ch2")
    parser.add_argument("--indexes", help="JSON, comma-separated, or whitespace-separated indexes")
    parser.add_argument("--indexes-file")
    return parser.parse_args()


def main():
    args = parse_args()

    if args.indexes is None and args.indexes_file is None:
        print("decode.py: provide --indexes or --indexes-file", file=sys.stderr)
        return 2

    try:
        sample_indexes = (
            parse_sample_indexes(args.indexes)
            if args.indexes is not None
            else load_sample_indexes(args.indexes_file)
        )
        data_values = load_channel_from_csv(args.csv, args.channel)
        result = decode_data_words(data_values, sample_indexes)
    except (DecodeError, KeyError, ValueError, OSError) as exc:
        print(f"decode.py: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
