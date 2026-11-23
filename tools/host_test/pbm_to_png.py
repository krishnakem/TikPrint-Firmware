#!/usr/bin/env python3
"""Turn a PBM picture (P4: 1 bit per dot, 1 = black) into a PNG, so it opens anywhere."""
import struct, sys, zlib

data = open(sys.argv[1], "rb").read()
magic, size, bits = data.split(b"\n", 2)
width, height = map(int, size.split())
row = (width + 7) // 8
raw = b"".join(b"\x00" + bytes(b ^ 0xFF for b in bits[y * row:(y + 1) * row]) for y in range(height))   # PNG: 1 = white

def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
