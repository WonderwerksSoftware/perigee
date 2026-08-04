#!/usr/bin/env python3

import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: pngs-to-icns.py PNG_DIRECTORY OUTPUT.icns", file=sys.stderr)
        return 2

    source_directory = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    icon_entries = (
        (b"ic07", 128),
        (b"ic08", 256),
        (b"ic09", 512),
        (b"ic10", 1024),
        (b"ic11", 32),
        (b"ic12", 64),
        (b"ic13", 256),
        (b"ic14", 512),
    )

    chunks = []
    for icon_type, size in icon_entries:
        payload = source_directory.joinpath(f"{size}.png").read_bytes()
        chunks.append(icon_type + struct.pack(">I", len(payload) + 8) + payload)

    body = b"".join(chunks)
    destination.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
