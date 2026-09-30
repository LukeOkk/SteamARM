#!/usr/bin/env python3
"""Clipboard through steamarm-wlmac, both ways, as a host wire client.

    clipboard_client.py SOCKET PASTEBOARD

PASTEBOARD is the named macOS pasteboard the compositor was started with
(WLMAC_PASTEBOARD), so the clipboard of the person at the Mac is never
touched. The Mac's text must reach the client as a wl_data_offer it can
receive; the client's wl_data_source must end up on the pasteboard.
"""
import array
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pasteboard import pasteboard  # noqa: E402

MIME = "text/plain;charset=utf-8"
FROM_MAC = "del Mac: ñandú €"
FROM_CLIENT = "del cliente Wayland: pingüino ✓"
def arg(*items):
    return struct.pack("<" + "I" * len(items), *items)


def string(value):
    data = value.encode() + b"\0"
    return arg(len(data)) + data + b"\0" * (-len(data) % 4)


def unstring(body, at=0):
    length = struct.unpack_from("<I", body, at)[0]
    return body[at + 4:at + 4 + length - 1].decode()


sock = socket.socket(socket.AF_UNIX)
sock.settimeout(8)
sock.connect(sys.argv[1])
name = sys.argv[2]
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
                if obj == 1 and op == 0:
                    raise RuntimeError("wl_display.error: " + repr(body))
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


send(1, 1, arg(2))
send(1, 0, arg(3))
globals_by_name = {}
while True:
    obj, op, body = receive()
    if obj == 2 and op == 0:
        globals_by_name[unstring(body, 4)] = struct.unpack_from("<I", body)[0]
    if obj == 3 and op == 0:
        break
send(2, 0, arg(globals_by_name["wl_seat"]) + string("wl_seat") + arg(5, 4))
send(2, 0, arg(globals_by_name["wl_data_device_manager"]) + string("wl_data_device_manager") + arg(3, 5))

# The Mac's text: offered as soon as the client has a data device.
pasteboard(name, FROM_MAC)
send(5, 1, arg(6, 4))
offer, mimes = None, []
while True:
    obj, op, body = receive()
    if obj == 6 and op == 0:
        offer = struct.unpack("<I", body)[0]
    elif offer is not None and obj == offer and op == 0:
        mimes.append(unstring(body))
    elif obj == 6 and op == 5:
        assert struct.unpack("<I", body)[0] == offer, "selection is not the offer"
        break
assert MIME in mimes, mimes
r, w = os.pipe()
send(offer, 1, string(MIME), w)
os.close(w)
got = b""
while chunk := os.read(r, 65536):
    got += chunk
os.close(r)
assert got.decode() == FROM_MAC, got

# The client's text: the compositor asks for it through a pipe.
send(5, 0, arg(7))
send(7, 0, string(MIME))
send(6, 1, arg(7, 0))
while True:
    obj, op, body = receive()
    if obj == 7 and op == 1:
        assert unstring(body) == MIME, body
        fd = fds.pop(0)
        os.write(fd, FROM_CLIENT.encode())
        os.close(fd)
        break
deadline = time.monotonic() + 5
while (now := pasteboard(name)) != FROM_CLIENT and time.monotonic() < deadline:
    time.sleep(0.2)
assert now == FROM_CLIENT, now
print(f"clipboard: the Mac's text reached the client ({len(FROM_MAC)} characters, {len(mimes)} types), "
      f"the client's selection reached the pasteboard ({len(FROM_CLIENT)} characters)")
sock.close()
