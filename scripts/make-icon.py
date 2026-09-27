#!/usr/bin/env python3
"""Draws the Steam ARM app icon and writes it as PNG.

Pure stdlib: the icon is rasterised into an RGBA buffer and encoded with zlib,
so the build needs no image library.
"""
import math
import struct
import sys
import zlib

SIZE = 1024
SS = 3  # supersampling factor per axis


def srgb(hex_str):
    v = int(hex_str, 16)
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


BG_TOP = srgb("1B2838")     # Steam-era graphite blue
BG_BOTTOM = srgb("0E1620")
ACCENT = srgb("66C0F4")     # light blue
WHITE = (255, 255, 255)


def rounded_square(x, y, size, radius):
    """Signed coverage test for a squircle-ish rounded square centred on the canvas."""
    half = size / 2
    cx = cy = size / 2
    dx = abs(x - cx) - (half - radius)
    dy = abs(y - cy) - (half - radius)
    if dx < 0 and dy < 0:
        return True
    dx = max(dx, 0.0)
    dy = max(dy, 0.0)
    return math.hypot(dx, dy) <= radius


def gear(x, y, cx, cy, outer, inner, teeth, tooth_depth):
    dx, dy = x - cx, y - cy
    r = math.hypot(dx, dy)
    if r > outer + tooth_depth or r < inner:
        return False
    if r <= outer:
        return True
    angle = math.atan2(dy, dx)
    # Teeth occupy half of each angular period.
    period = 2 * math.pi / teeth
    phase = (angle % period) / period
    return phase < 0.5 and r <= outer + tooth_depth


def render():
    buf = bytearray(SIZE * SIZE * 4)
    cx = cy = SIZE / 2

    gear_outer = SIZE * 0.30
    gear_inner = SIZE * 0.20
    tooth = SIZE * 0.055
    hub = SIZE * 0.095
    radius = SIZE * 0.225

    for py in range(SIZE):
        for px in range(SIZE):
            acc_r = acc_g = acc_b = acc_a = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS

                    if not rounded_square(x, y, SIZE, radius):
                        continue

                    t = y / SIZE
                    r = int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t)
                    g = int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t)
                    b = int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t)

                    if gear(x, y, cx, cy, gear_outer, gear_inner, 12, tooth):
                        r, g, b = WHITE
                    else:
                        d = math.hypot(x - cx, y - cy)
                        if d <= hub:
                            r, g, b = ACCENT

                    acc_r += r
                    acc_g += g
                    acc_b += b
                    acc_a += 255

            n = SS * SS
            i = (py * SIZE + px) * 4
            if acc_a:
                buf[i + 0] = acc_r // n
                buf[i + 1] = acc_g // n
                buf[i + 2] = acc_b // n
                buf[i + 3] = acc_a // n
    return bytes(buf)


def write_png(path, rgba):
    raw = bytearray()
    stride = SIZE * 4
    for y in range(SIZE):
        raw.append(0)  # filter type: none
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as fh:
        fh.write(png)


if __name__ == "__main__":
    write_png(sys.argv[1] if len(sys.argv) > 1 else "icon.png", render())
    print("wrote icon")
