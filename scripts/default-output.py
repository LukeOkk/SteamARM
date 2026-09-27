#!/usr/bin/env python3
"""Print the name of the Mac's default audio output device (CoreAudio), for
scripts/audio.sh to pick the matching PulseAudio sink."""
import ctypes
import ctypes.util

ca = ctypes.CDLL(ctypes.util.find_library("CoreAudio"))
cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))


class Addr(ctypes.Structure):
    _fields_ = [("selector", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("element", ctypes.c_uint32)]


def fourcc(s):
    return int.from_bytes(s.encode(), "big")


SYSTEM_OBJECT = 1
dev = ctypes.c_uint32(0)
size = ctypes.c_uint32(4)
addr = Addr(fourcc("dOut"), fourcc("glob"), 0)
if ca.AudioObjectGetPropertyData(SYSTEM_OBJECT, ctypes.byref(addr), 0, None, ctypes.byref(size), ctypes.byref(dev)) == 0:
    name = ctypes.c_void_p()
    size = ctypes.c_uint32(ctypes.sizeof(name))
    addr = Addr(fourcc("lnam"), fourcc("glob"), 0)
    if ca.AudioObjectGetPropertyData(dev, ctypes.byref(addr), 0, None, ctypes.byref(size), ctypes.byref(name)) == 0:
        buf = ctypes.create_string_buffer(512)
        cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
        if cf.CFStringGetCString(name, buf, 512, 0x08000100):
            print(buf.value.decode())
