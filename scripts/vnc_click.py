#!/usr/bin/env python3
"""Click (or type) on the guest X display through its VNC server (no deps).

    scripts/vnc_click.py <x> <y> [host] [port]          left click at x,y
    scripts/vnc_click.py --key <keysym-hex> [host] [port]  press+release a key
"""
import socket, struct, sys, time


def connect(host, port):
    s = socket.create_connection((host, port), timeout=20)
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    import vnc_auth
    vnc_auth.handshake(s)
    s.sendall(b"\x01")                         # ClientInit, shared
    hdr = s.recv(24)
    name_len = struct.unpack(">I", hdr[20:24])[0]
    s.recv(name_len)
    return s


def main():
    a = sys.argv[1:]
    if a and a[0] == "--key":
        key = int(a[1], 16)
        host = a[2] if len(a) > 2 else "127.0.0.1"
        port = int(a[3]) if len(a) > 3 else 5901
        s = connect(host, port)
        for down in (1, 0):
            s.sendall(struct.pack(">BBxxI", 4, down, key))
            time.sleep(0.05)
    else:
        x, y = int(a[0]), int(a[1])
        host = a[2] if len(a) > 2 else "127.0.0.1"
        port = int(a[3]) if len(a) > 3 else 5901
        s = connect(host, port)
        for mask in (0, 1, 0):
            s.sendall(struct.pack(">BBHH", 5, mask, x, y))
            time.sleep(0.08)
    time.sleep(0.2)
    s.close()


if __name__ == "__main__":
    main()
