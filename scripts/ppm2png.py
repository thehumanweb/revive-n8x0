#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png [scale]: a binary P6 PPM to a PNG (standard library only)."""
import struct
import sys
import zlib

d = open(sys.argv[1], "rb").read()
_, size, _, rgb = d.split(b"\n", 3)
w, h = map(int, size.split())
k = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = []
for y in range(h):
    row = b"".join(rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * k for x in range(w))
    rows += [b"\0" + row] * k


def chunk(t, b):
    c = struct.pack(">I", len(b)) + t + b
    return c + struct.pack(">I", zlib.crc32(t + b))


open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * k, h * k, 8, 2, 0, 0, 0)) +
                              chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
