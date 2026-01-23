#!/usr/bin/env python3
import argparse
import os
import struct
import sys


def bf16_to_f32(u16):
    # bfloat16 stores the high 16 bits of IEEE754 float32.
    u32 = u16 << 16
    return struct.unpack("<f", struct.pack("<I", u32))[0]


def iter_cbf16_samples(data):
    for real_u16, imag_u16 in struct.iter_unpack("<HH", data):
        yield bf16_to_f32(real_u16), bf16_to_f32(imag_u16)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Parse cbf16 binary dumps (real bf16 + imag bf16 per sample)."
    )
    parser.add_argument("input", help="Path to .cbf16 file")
    parser.add_argument("--skip", type=int, default=0, help="Complex samples to skip")
    parser.add_argument("--count", type=int, default=None, help="Complex samples to read")
    return parser.parse_args()


def main():
    args = parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    if len(data) % 4 != 0:
        print(
            "warning: file size is not a multiple of 4 bytes; trailing bytes ignored",
            file=sys.stderr,
        )
        data = data[: len(data) - (len(data) % 4)]

    total_samples = len(data) // 4
    if args.skip < 0 or args.skip > total_samples:
        print("error: --skip out of range", file=sys.stderr)
        return 1

    count = total_samples - args.skip if args.count is None else args.count
    if count < 0:
        print("error: --count must be >= 0", file=sys.stderr)
        return 1

    start = args.skip * 4
    end = start + count * 4
    data = data[start:end]

    for real, imag in iter_cbf16_samples(data):
        sys.stdout.write(f"{real} {imag}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
