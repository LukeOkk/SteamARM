#!/usr/bin/env python3
"""A stand-in for Android's logd write socket, on the host (no VM).

Android programs (liblog) send every log line as one datagram to
/dev/socket/logdw. Under lxrun that guest path is <root>/dev/socket/logdw on
the Mac, so a host process bound there receives what ART, bionic's linker
and the rest would send to logcat, which is where their errors go
(benchmarks/stage25-android-userspace.txt).

  tests/android/logd.py <android root> [--out FILE] [--seconds N]

Prints "<prio> <tag>: <message>" per record until killed (or N seconds).
Stops only itself; removes the socket it created.
"""
import os
import signal
import socket
import struct
import sys
import time

PRIO = "??VDIWEFS"
LOG_ID = {0: "main", 1: "radio", 2: "events", 3: "system", 4: "crash", 5: "stats", 6: "security", 7: "kernel"}


def decode(pkt):
    # android_log_header_t: uint8 id, uint16 tid, uint32 sec, uint32 nsec
    if len(pkt) < 11:
        return "short record (%d bytes)" % len(pkt)
    lid, tid, sec, nsec = struct.unpack_from("<BHII", pkt, 0)
    body = pkt[11:]
    if lid in (2, 5, 6):  # binary event logs
        return "%s tid %d: binary event (%d bytes)" % (LOG_ID.get(lid, lid), tid, len(body))
    if not body:
        return "%s tid %d: empty" % (LOG_ID.get(lid, lid), tid)
    prio = body[0]
    rest = body[1:].split(b"\0")
    tag = rest[0].decode("utf-8", "replace") if rest else ""
    msg = rest[1].decode("utf-8", "replace") if len(rest) > 1 else ""
    p = PRIO[prio] if prio < len(PRIO) else str(prio)
    return "%s %s tid %d %s: %s" % (time.strftime("%H:%M:%S", time.localtime(sec)), p, tid, tag, msg)


def main(argv):
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    root = argv[0]
    out = None
    seconds = None
    a = argv[1:]
    while a:
        if a[0] == "--out" and len(a) > 1:
            out = open(a[1], "a", buffering=1)
            a = a[2:]
        elif a[0] == "--seconds" and len(a) > 1:
            seconds = float(a[1])
            a = a[2:]
        else:
            print("unknown option %s" % a[0], file=sys.stderr)
            return 2
    d = os.path.join(root, "dev", "socket")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "logdw")
    if os.path.exists(path):
        os.unlink(path)
    # SIGTERM (the test's kill) ends it like ^C: the socket file goes too.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(path)
    os.chmod(path, 0o666)
    if seconds:
        s.settimeout(0.5)
    end = time.time() + seconds if seconds else None
    try:
        while end is None or time.time() < end:
            try:
                pkt = s.recv(65536)
            except socket.timeout:
                continue
            line = decode(pkt)
            print(line, file=out or sys.stdout, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        try:
            os.unlink(path)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
