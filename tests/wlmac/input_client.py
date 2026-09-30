#!/usr/bin/env python3
"""Native wire client for compositor synthetic NSEvent path (no GUI automation)."""
import array
import os
import socket
import struct
import sys
import time


def arg(*items):
    return struct.pack("<" + "I" * len(items), *items)


def string(value):
    data = value.encode() + b"\0"
    return arg(len(data)) + data + b"\0" * (-len(data) % 4)


sock = socket.socket(socket.AF_UNIX)
sock.settimeout(8)
sock.connect(sys.argv[1])
incoming = bytearray()
fds = []


def send(obj, op, body=b"", fd=None):
    header = arg(obj, ((len(body) + 8) << 16) | op)
    ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))] if fd is not None else []
    sock.sendmsg([header + body], ancillary)


def receive():
    while True:
        if len(incoming) >= 8:
            obj, header = struct.unpack_from("<II", incoming)
            length, op = header >> 16, header & 65535
            if len(incoming) >= length:
                body = bytes(incoming[8:length])
                del incoming[:length]
                return obj, op, body
        data, anc, *_ = sock.recvmsg(65536, socket.CMSG_SPACE(64))
        if not data:
            raise RuntimeError("compositor disconnected")
        incoming.extend(data)
        for level, kind, payload in anc:
            if (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS):
                received = array.array("i")
                received.frombytes(payload[: len(payload) // 4 * 4])
                fds.extend(received)


def drain_until(predicate):
    while True:
        obj, op, body = receive()
        if obj == 1 and op == 0:
            raise RuntimeError("wl_display.error: " + repr(body))
        if predicate(obj, op, body):
            return body


send(1, 1, arg(2))
send(1, 0, arg(3))
globals_by_name = {}


def registry(obj, op, body):
    if obj == 2 and op == 0:
        name, length = struct.unpack_from("<II", body)
        iface = body[8:8 + length - 1].decode()
        version = struct.unpack_from("<I", body, 8 + ((length + 3) & ~3))[0]
        globals_by_name[iface] = (name, version)
    return obj == 3 and op == 0


drain_until(registry)
for obj, iface, version in ((4, "wl_compositor", 4), (5, "wl_shm", 1), (6, "xdg_wm_base", 1), (7, "wl_seat", 5)):
    name, offered = globals_by_name[iface]
    send(2, 0, arg(name) + string(iface) + arg(min(offered, version), obj))
send(7, 0, arg(8))
send(7, 1, arg(9))

width = height = 64
# A host process: no memfd on macOS, an unlinked temporary file instead.
import tempfile
_fd, _path = tempfile.mkstemp(prefix="wlmac-input-")
os.unlink(_path)
shm = _fd
os.ftruncate(shm, width * height * 4)
os.write(shm, b"\x20\x80\xc0\xff" * (width * height))
send(5, 0, arg(10, width * height * 4), shm)
send(10, 0, arg(11, 0, width, height, width * 4, 1))
send(4, 0, arg(12))
send(6, 2, arg(13, 12))
send(13, 1, arg(14))
send(14, 2, string("WLMAC-INPUT"))
send(12, 6)


def configured(obj, op, body):
    return obj == 13 and op == 0


serial = struct.unpack("<I", drain_until(configured))[0]
send(13, 4, arg(serial))
send(12, 1, arg(11, 0, 0))
send(12, 6)

got = set()
deadline = time.monotonic() + 8
while time.monotonic() < deadline and got != {"enter", "motion", "button", "key"}:
    obj, op, body = receive()
    if obj == 1 and op == 0:
        raise RuntimeError("wl_display.error: " + repr(body))
    if obj == 9 and op == 0 and fds:
        os.close(fds.pop(0))
    if obj == 8 and op == 0:
        assert struct.unpack_from("<I", body, 4)[0] == 12
        got.add("enter")
    if obj == 8 and op == 2:
        got.add("motion")
    if obj == 8 and op == 3:
        assert struct.unpack_from("<I", body, 8)[0] == 0x110
        got.add("button")
    if obj == 9 and op == 3:
        assert struct.unpack_from("<I", body, 8)[0] == 30  # macOS keyCode 0, Linux KEY_A
        got.add("key")

assert got == {"enter", "motion", "button", "key"}, got
print("input: pointer enter/motion/BTN_LEFT(0x110), keyboard KEY_A(30) received")
sock.close()
os.close(shm)
