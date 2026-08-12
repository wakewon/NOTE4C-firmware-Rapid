#!/usr/bin/env python3
"""Extract an SSD2683 MTP binary from a Note4C serial log."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


LINE_RE = re.compile(r"\[SSD2683_MTP\]\s+([0-9A-Fa-f]{4}):\s+((?:[0-9A-Fa-f]{2}\s*)+)")
MTP_SIZE = 3840


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def extract(log_text: str) -> bytes:
    image = bytearray(MTP_SIZE)
    present = bytearray(MTP_SIZE)

    for match in LINE_RE.finditer(log_text):
        offset = int(match.group(1), 16)
        chunk = bytes.fromhex(match.group(2))
        end = offset + len(chunk)
        if offset >= MTP_SIZE or end > MTP_SIZE:
            raise ValueError(f"MTP line outside 0..{MTP_SIZE - 1}: 0x{offset:04X}")
        image[offset:end] = chunk
        present[offset:end] = b"\x01" * len(chunk)

    missing = [index for index, found in enumerate(present) if not found]
    if missing:
        first = missing[0]
        raise ValueError(
            f"incomplete dump: missing {len(missing)} bytes, first at 0x{first:04X}"
        )
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial_log", type=Path, help="captured idf.py monitor output")
    parser.add_argument("output", type=Path, help="3840-byte output image")
    args = parser.parse_args()

    data = extract(args.serial_log.read_text(encoding="utf-8", errors="replace"))
    args.output.write_bytes(data)
    print(f"wrote {len(data)} bytes to {args.output} (fnv1a32={fnv1a32(data):08X})")


if __name__ == "__main__":
    main()
