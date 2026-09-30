#!/usr/bin/env python3
"""Real pointer clicks and keys into an X window, with XTEST: to the X server
they are the same as a person's (unlike XSendEvent, whose send_event bit
Weston's X11 backend refuses for buttons, see xsend_motion.py). SteamARM's X
server has XTEST when scripts/run-x11-native.sh started it with
STEAMARM_X11_XTEST=1 (the default); exit status 3 when it has none.

  tests/android/xtest_input.py <display> <window id> click <x> <y>
  tests/android/xtest_input.py <display> <window id> key <keysym name>

x and y are in the window (Weston's output: Android's screen). The pointer is
moved there first. A key goes to the window with the X input focus, which is
given to this window first (XSetInputFocus: no click reaches Android)."""
import ctypes
import sys
import time

X = ctypes.CDLL("/opt/homebrew/lib/libX11.dylib")
XT = ctypes.CDLL("/opt/homebrew/lib/libXtst.dylib")
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XDefaultRootWindow.restype = ctypes.c_ulong
X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
X.XTranslateCoordinates.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                    ctypes.POINTER(ctypes.c_ulong)]
X.XStringToKeysym.restype = ctypes.c_ulong
X.XStringToKeysym.argtypes = [ctypes.c_char_p]
X.XKeysymToKeycode.restype = ctypes.c_ubyte
X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
X.XCloseDisplay.argtypes = [ctypes.c_void_p]
X.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
XT.XTestQueryExtension.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_int)] * 4
XT.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
XT.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
XT.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]


def main():
    if len(sys.argv) < 4 or sys.argv[3] not in ("click", "key"):
        sys.exit(__doc__)
    d = X.XOpenDisplay(sys.argv[1].encode())
    if not d:
        sys.exit("cannot open display " + sys.argv[1])
    a, b, c, e = (ctypes.c_int() for _ in range(4))
    if not XT.XTestQueryExtension(d, a, b, c, e):
        print("no XTEST on %s" % sys.argv[1])
        sys.exit(3)
    win = int(sys.argv[2], 0)

    def settle():
        X.XSync(d, 0)
        time.sleep(0.15)

    if sys.argv[3] == "key":
        sym = X.XStringToKeysym(sys.argv[4].encode())
        code = X.XKeysymToKeycode(d, sym) if sym else 0
        if not code:
            sys.exit("no keycode for keysym %r" % sys.argv[4])
        X.XSetInputFocus(d, win, 2, 0)          # RevertToParent, CurrentTime
        settle()
        XT.XTestFakeKeyEvent(d, code, 1, 0)
        settle()
        XT.XTestFakeKeyEvent(d, code, 0, 0)
        settle()
        print("key %s (keycode %d)" % (sys.argv[4], code))
    else:
        x, y = int(sys.argv[4]), int(sys.argv[5])
        rx, ry, child = ctypes.c_int(), ctypes.c_int(), ctypes.c_ulong()
        if not X.XTranslateCoordinates(d, win, X.XDefaultRootWindow(d), x, y, rx, ry, child):
            sys.exit("window %s is not on this screen" % sys.argv[2])
        XT.XTestFakeMotionEvent(d, -1, rx.value, ry.value, 0)
        settle()
        XT.XTestFakeButtonEvent(d, 1, 1, 0)
        settle()
        XT.XTestFakeButtonEvent(d, 1, 0, 0)
        settle()
        print("click at %d,%d (root %d,%d)" % (x, y, rx.value, ry.value))
    X.XCloseDisplay(d)


main()
