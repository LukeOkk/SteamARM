#!/usr/bin/env python3
"""Synthetic pointer motion into an X window, with XSendEvent (SteamARM's X
server has no XTEST): one EnterNotify, then N MotionNotify across the
window. Weston's X11 backend turns them into wl_pointer events for the
surface under them; hwcomposer.waydroid writes those into its
/dev/input/wl_pointer_events FIFO (tests/android/run.sh, display section).
No buttons or keys: Weston's X11 backend asserts on a synthetic button event
(libweston/backend-x11/x11.c:1349, the send_event bit in response_type) and
aborts (MEASURED, benchmarks/stage28-android-reliability.txt).

  tests/android/xsend_motion.py <display> <window id> [motions]"""
import ctypes
import sys
import time

X = ctypes.CDLL("/opt/homebrew/lib/libX11.dylib")
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
X.XFlush.argtypes = [ctypes.c_void_p]
X.XCloseDisplay.argtypes = [ctypes.c_void_p]
X.XDefaultRootWindow.restype = ctypes.c_ulong
X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]


class PointerEvent(ctypes.Structure):   # XMotionEvent / XCrossingEvent: the shared prefix
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
                ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong), ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("x_root", ctypes.c_int), ("y_root", ctypes.c_int), ("state", ctypes.c_uint),
                ("detail", ctypes.c_uint), ("same_screen", ctypes.c_int), ("pad", ctypes.c_long * 16)]


MOTION, ENTER = 6, 7
MASK = {MOTION: 1 << 6, ENTER: 1 << 4}
d = X.XOpenDisplay(sys.argv[1].encode())
if not d:
    sys.exit("cannot open display " + sys.argv[1])
win = int(sys.argv[2], 0)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
root = X.XDefaultRootWindow(d)
now = int(time.time() * 1000) & 0xffffffff


def send(kind, x, y, t):
    e = PointerEvent()
    e.type, e.send_event, e.display, e.window, e.root = kind, 1, d, win, root
    e.time, e.x, e.y, e.x_root, e.y_root, e.same_screen = t, x, y, x, y, 1
    X.XSendEvent(d, win, 1, MASK[kind], ctypes.byref(e))
    X.XFlush(d)


send(ENTER, 300, 250, now)
for i in range(n):
    send(MOTION, 300 + i * 5, 250 + i * 3, now + 16 * (i + 1))
    time.sleep(0.02)
X.XCloseDisplay(d)
print("sent: enter and %d motions to 0x%x" % (n, win))
