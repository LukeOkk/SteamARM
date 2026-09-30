#!/usr/bin/env python3
"""Check compositor PNG with Python's standard library only."""
import collections
import struct
import sys
import zlib

raw = open(sys.argv[1], "rb").read()
assert raw.startswith(b"\x89PNG\r\n\x1a\n")
pos = 8
payload = bytearray()
while pos < len(raw):
    size = struct.unpack_from(">I", raw, pos)[0]
    kind = raw[pos + 4:pos + 8]
    chunk = raw[pos + 8:pos + 8 + size]
    if kind == b"IHDR":
        width, height, depth, color, *_ = struct.unpack(">IIBBBBB", chunk)
    elif kind == b"IDAT":
        payload += chunk
    elif kind == b"IEND":
        break
    pos += size + 12
assert depth == 8 and color in (2, 6), (depth, color)
bpp = 3 if color == 2 else 4
scan = zlib.decompress(payload)
counts = collections.Counter()
at = 0
prior = bytearray(width * bpp)
for y in range(height):
    filt = scan[at]
    at += 1
    row = bytearray(scan[at:at + width * bpp])
    at += len(row)
    for x in range(len(row)):
        left = row[x - bpp] if x >= bpp else 0
        above = prior[x]
        upper_left = prior[x - bpp] if x >= bpp else 0
        if filt == 1:
            row[x] = (row[x] + left) & 255
        elif filt == 2:
            row[x] = (row[x] + above) & 255
        elif filt == 3:
            row[x] = (row[x] + ((left + above) // 2)) & 255
        elif filt == 4:
            p = left + above - upper_left
            distances = (abs(p - left), abs(p - above), abs(p - upper_left))
            row[x] = (row[x] + (left, above, upper_left)[distances.index(min(distances))]) & 255
        else:
            assert filt == 0, filt
    for x in range(width):
        counts[tuple(row[x * bpp:x * bpp + 3])] += 1
    prior = row

expected = ((32, 128, 192), (192, 64, 32), (32, 192, 64), (224, 224, 224))
for rgb in expected:
    n = counts[rgb]
    assert width * height * 0.22 <= n <= width * height * 0.26, (rgb, n, width * height)
print(f"PNG {width}x{height}: " + ", ".join(f"{rgb}={counts[rgb]}" for rgb in expected))
