#!/usr/bin/env python3
"""Grab one frame from a VNC server (RFB 3.8, security None, Raw) as a PNG.

Used to check what the runtime-hosted Xvnc is showing without a VNC viewer:
    scripts/vnc_snapshot.py [host] [port] [out.png]
"""
import socket, struct, sys, zlib

def recvn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("server closed")
        b += c
    return b

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5901
    out = sys.argv[3] if len(sys.argv) > 3 else "vnc.png"
    s = socket.create_connection((host, port), timeout=20)
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    import vnc_auth
    vnc_auth.handshake(s)
    s.sendall(b"\x01")                      # shared
    w, h = struct.unpack(">HH", recvn(s, 4))
    recvn(s, 16)
    name_len = struct.unpack(">I", recvn(s, 4))[0]
    name = recvn(s, name_len).decode(errors="replace")
    # 32bpp, depth 24, little endian, true colour, R<<16 G<<8 B
    s.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    s.sendall(struct.pack(">BxHi", 2, 1, 0))  # SetEncodings: Raw
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
    fb = bytearray(w * h * 4)
    while True:
        t = recvn(s, 1)[0]
        if t != 0:
            if t == 2:
                continue            # bell
            if t == 3:
                l = struct.unpack(">xxxI", recvn(s, 7))[0]; recvn(s, l); continue
            continue
        nrect = struct.unpack(">xH", recvn(s, 3))[0]
        for _ in range(nrect):
            x, y, rw, rh, enc = struct.unpack(">HHHHi", recvn(s, 12))
            if enc != 0:
                raise SystemExit(f"unexpected encoding {enc}")
            data = recvn(s, rw * rh * 4)
            for row in range(rh):
                o = ((y + row) * w + x) * 4
                fb[o:o + rw * 4] = data[row * rw * 4:(row + 1) * rw * 4]
        break
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = fb[y * w * 4:(y + 1) * w * 4]
        for x in range(w):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            raw += bytes((r, g, b))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(out, "wb").write(png)
    nonblack = sum(1 for i in range(0, len(fb), 4 * 97) if fb[i] or fb[i + 1] or fb[i + 2])
    print(f"{name}: {w}x{h} -> {out} (sampled non-black pixels: {nonblack})")

if __name__ == "__main__":
    main()
