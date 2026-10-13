#!/usr/bin/env python3
"""Two numbers for a captured frame (the shim's LXRT_VK_SNAPSHOT frame.ppm):
sharpness (the variance of a 3x3 Laplacian of the luma, higher = crisper)
and, given a second frame of the same size, the mean absolute difference of
the luma (0..255) and the fraction of pixels that differ by more than 8.

    ppm_compare.py FRAME.ppm [OTHER.ppm] [left=N]

left=N skips the N leftmost columns (a sample's UI panel, whose text would
dominate the sharpness). The top-right 13 % (the shim blacks it out) and a
one-pixel border are always skipped.
Pure Python (no PIL/numpy); a half-size 1080p frame takes a few seconds.
Prints one line: "sharpness A [sharpness B meandiff D differing F%]".
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields = []
    pos = 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6":
        sys.exit(f"{path}: not a P6 ppm")
    w, h = int(fields[1]), int(fields[2])
    px = data[pos:pos + w * h * 3]
    if len(px) < w * h * 3:
        sys.exit(f"{path}: short file")
    luma = [(px[i] * 299 + px[i + 1] * 587 + px[i + 2] * 114) // 1000 for i in range(0, w * h * 3, 3)]
    return w, h, luma


def sharpness(w, h, l, left=0):
    # The top-right 13 % (the shim blacks it out) and a border are skipped.
    top = h * 13 // 100
    total = 0.0
    total2 = 0.0
    n = 0
    for y in range(max(1, top), h - 1):
        row = y * w
        for x in range(max(1, left), w - 1):
            v = 4 * l[row + x] - l[row + x - 1] - l[row + x + 1] - l[row - w + x] - l[row + w + x]
            total += v
            total2 += v * v
            n += 1
    mean = total / n
    return total2 / n - mean * mean


def main(argv):
    files = [a for a in argv[1:] if not a.startswith("left=")]
    left = 0
    for a in argv[1:]:
        if a.startswith("left="):
            left = int(a[5:])
    if not files:
        sys.exit(__doc__.strip())
    w, h, a = read_ppm(files[0])
    out = f"sharpness {sharpness(w, h, a, left):.1f}"
    if len(files) > 1:
        w2, h2, b = read_ppm(files[1])
        if (w2, h2) != (w, h):
            sys.exit(f"sizes differ: {w}x{h} vs {w2}x{h2}")
        top = h * 13 // 100
        diff = 0
        big = 0
        n = 0
        for y in range(top, h):
            for x in range(left, w):
                d = abs(a[y * w + x] - b[y * w + x])
                diff += d
                if d > 8:
                    big += 1
                n += 1
        out += f" sharpness {sharpness(w2, h2, b, left):.1f} meandiff {diff / n:.2f} differing {100.0 * big / n:.1f}%"
    if left:
        out += f" (left {left} px skipped)"
    print(out)


if __name__ == "__main__":
    main(sys.argv)
