#!/usr/bin/env python3
"""The guest pid of a host process under LXRT_SMALL_IDS=1 (runtime/ids.c),
read from the runtime's shared id table /tmp/lxrt-ids-<uid>: struct id_table
{ uint32 cursor; struct { int32 pid, thread; uint32 generation; } entries[65536]; }.
Prints the host pid itself when the process has no small id (the mode off),
so a test can compare what a guest tool prints either way.

  tests/android/guest_pid.py <host pid>"""
import os
import struct
import sys

pid = int(sys.argv[1])
try:
    with open("/tmp/lxrt-ids-%d" % os.getuid(), "rb") as f:
        table = f.read()
except OSError:
    table = b""
guest = pid
for gid in range(2, (len(table) - 4) // 12):
    hp, thread, _ = struct.unpack_from("<iiI", table, 4 + gid * 12)
    if hp == pid and thread == 0:
        guest = gid
        break
print(guest)
