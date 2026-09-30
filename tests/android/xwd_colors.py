#!/usr/bin/env python3
"""Read an X window dump (xwd -id <window>) and report its colours.

The Mac's ImageMagick has no XWD reader, so this parses the format itself:
the XWDFileHeader (25 big-endian CARD32s, X11/XWDFile.h), the window name,
the colormap, then ZPixmap rows in the header's byte order. TrueColor 24/32
bits per pixel only, which is what XQuartz gives.

    xwd_colors.py DUMP [--ppm OUT.ppm] [--expect RRGGBB ...] [--min-frac F]

Prints "WxH", the most common colours with their share, and for each
--expect colour the number of pixels of exactly that colour. Exit status 0
when every expected colour covers at least --min-frac of the image
(default 0.01), 1 otherwise, 2 on a format this does not read.
(docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Display".)
"""
import argparse
import collections
import struct
import sys


def read_xwd(path):
    with open(path, 'rb') as f:
        data = f.read()
    h = struct.unpack('>25I', data[:100])
    (header_size, version, fmt, depth, width, height, _xoff, byte_order, _unit,
     _bit_order, _pad, bpp, bpl, vclass, rmask, gmask, bmask, _bits_rgb,
     _cmap_entries, ncolors) = h[:20]
    if version != 7 or fmt != 2 or bpp not in (24, 32) or vclass not in (4, 5):
        raise ValueError(f'unsupported xwd: version {version} format {fmt} bpp {bpp} class {vclass}')
    name = data[100:header_size].split(b'\0', 1)[0].decode('latin-1')
    off = header_size + ncolors * 12
    endian = '<' if byte_order == 0 else '>'
    step = bpp // 8

    def shift(m):
        s = 0
        while m and not m & 1:
            m >>= 1
            s += 1
        return s
    rs, gs, bs = shift(rmask), shift(gmask), shift(bmask)
    px = []
    for y in range(height):
        row = data[off + y * bpl: off + y * bpl + width * step]
        for x in range(width):
            if step == 4:
                (v,) = struct.unpack_from(endian + 'I', row, x * 4)
            else:
                b = row[x * 3: x * 3 + 3]
                v = int.from_bytes(b, 'little' if byte_order == 0 else 'big')
            px.append((((v & rmask) >> rs) & 255, ((v & gmask) >> gs) & 255, ((v & bmask) >> bs) & 255))
    return name, width, height, depth, px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--ppm')
    ap.add_argument('--expect', nargs='*', default=[])
    ap.add_argument('--min-frac', type=float, default=0.01)
    a = ap.parse_args()
    try:
        name, w, h, depth, px = read_xwd(a.dump)
    except (ValueError, struct.error) as e:
        print(f'xwd_colors: {e}')
        return 2
    print(f'{w}x{h} depth {depth} window "{name}"')
    if a.ppm:
        with open(a.ppm, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (w, h))
            f.write(bytes(c for p in px for c in p))
    hist = collections.Counter(px)
    total = len(px) or 1
    for (r, g, b), n in hist.most_common(8):
        print(f'  {r:02x}{g:02x}{b:02x}  {n:8d}  {100.0 * n / total:5.1f}%')
    ok = True
    for e in a.expect:
        want = (int(e[0:2], 16), int(e[2:4], 16), int(e[4:6], 16))
        n = hist.get(want, 0)
        good = n >= a.min_frac * total
        ok &= good
        print(f'  expect {e}: {n} pixels ({100.0 * n / total:.1f}%) {"ok" if good else "MISSING"}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
