#!/usr/bin/env python3
"""A stand-in for steamarm-inputd (tools/inputd/PROTOCOL.md) for tests/elf:
one Xbox 360 pad at /tmp/lxrt-input/event0 that, once a client has connected
and taken its snapshot, presses A, moves the left stick and releases A.
Rumble records are printed as "rumble strong=S weak=W ms=M"."""
import os, socket, struct, sys, time, select

# LXRT_INPUT_DIR: a private /dev/input (runtime/evdev.c), as the guest gets it.
D = os.environ.get("LXRT_INPUT_DIR", "/tmp/lxrt-input")
os.makedirs(D + "/meta", exist_ok=True)
for f in ("event0", "meta/event0"):
    try: os.unlink(os.path.join(D, f))
    except OSError: pass
META = """name Microsoft X-Box 360 pad
id 0003 045e 028e 0114
phys usb-steamarm-0/input0
uniq
key 304 305 307 308 310 311 314 315 316 317 318
abs 0 -32768 32767 16 128 0
abs 1 -32768 32767 16 128 0
abs 2 0 255 0 0 0
abs 3 -32768 32767 16 128 0
abs 4 -32768 32767 16 128 0
abs 5 0 255 0 0 0
abs 16 -1 1 0 0 0
abs 17 -1 1 0 0 0
ff 80 81 88 89 90 96
effects 16
"""
with open(D + "/meta/.event0", "w") as f:
    f.write(META)
os.rename(D + "/meta/.event0", D + "/meta/event0")
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(D + "/event0")
srv.listen(8)
print("ready", flush=True)

def ev(t, c, v):
    now = time.time()
    return struct.pack("<qqHHi", int(now), int((now % 1) * 1e6), t, c, v)

def frame(*evs):
    return b"".join(ev(*e) for e in evs) + ev(0, 0, 0)

deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 20)
clients = []
tick, phase = time.time(), 0
while time.time() < deadline:
    r, _, _ = select.select([srv] + clients, [], [], 0.05)
    for s in r:
        if s is srv:
            c, _ = srv.accept()
            # snapshot: left stick already at x=1000, trigger at 7
            c.sendall(frame((3, 0, 1000), (3, 2, 7)))
            if "--once" in sys.argv:
                time.sleep(0.3)
                c.sendall(frame((1, 304, 1)))
                c.sendall(frame((3, 0, -20000), (3, 1, 12345)))
                c.sendall(frame((1, 304, 0)))
            clients.append(c)
        else:
            data = s.recv(24)
            if len(data) < 24:
                clients.remove(s); s.close(); continue
            magic, strong, weak, ms = struct.unpack("<IHHI", data[:12])
            if magic == 0x4c424d52:
                print("rumble strong=%d weak=%d ms=%d" % (strong, weak, ms), flush=True)
    # Without --once: A held and the stick left for half a second, then
    # released, over and over (for pollers such as XInput).
    if "--once" not in sys.argv and time.time() - tick > 0.5:
        tick, phase = time.time(), phase ^ 1
        f = frame((1, 304, phase), (3, 0, -20000 if phase else 1000))
        for c in list(clients):
            try: c.sendall(f)
            except OSError: clients.remove(c)
os.unlink(D + "/event0"); os.unlink(D + "/meta/event0")
