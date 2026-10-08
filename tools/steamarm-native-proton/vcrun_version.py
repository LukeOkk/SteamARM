#!/usr/bin/env python3
"""The Visual C++ runtime version Proton's builtins report, raised to 14.44.

Proton replaces the Microsoft Visual C++ 2015-2022 redistributable with
Wine's own msvcp140/vcruntime140/... builtins and says in the registry
(HKLM\\Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\<arch>) and in
the DLLs' version resources which redistributable that stands for: 14.42.34433
in the Proton ARM64 this tool copies. Valve raises it when games start to
check for newer ones; Unreal Engine's bootstrap launcher checks both, and
Minecraft Dungeons II's wants 14.42.34438 or later ("The following
component(s) are required to run this program: Microsoft Visual C++ 2015-2022
Redistributable (arm64)" -- it asks for the arm64 one because IsWow64Process2
says the machine is ARM64). The redistributable's installers it offers are
32-bit x86 programs, which the native tool cannot run (no WoW64 on macOS), so
the box could not be got past at all.

This script claims 14.44.35211 (Visual Studio 2022 17.14's redistributable)
where the version is lower, exactly what Proton itself does at each of its
bumps; the builtins are the same code.

  vcrun_version.py dist DIST   the tool's copy of Proton: the builtins'
                               version resources in files/lib/wine/*-windows
                               and the default prefix's registry. A changed
                               default_pfx*/system.reg makes Proton copy the
                               builtins into existing prefixes again at their
                               next start (its prefix_info has that file's
                               mtime).
  vcrun_version.py prefix PFX  an existing prefix's registry (system.reg),
                               which Proton never takes from the default
                               prefix again; run by the tool before Proton.

Only for a Proton copy of our own: Wine reads a builtin's version resource
from Proton's lib/wine, not from the prefix's copy in system32, so the x86
tool (Valve's Proton run in place) cannot be helped this way; there the
launcher's own offer works -- its vc_redist.x64.exe, a 32-bit installer,
runs under WoW64 on that path (benchmarks/stage62, 11).

Prints what it changed; exit 0 also when there was nothing to change.
"""
import os
import re
import struct
import sys

TARGET = (14, 44, 35211, 0)
DLLS = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
        "vcomp140.dll", "vccorlib140.dll")
FIXED_SIG = struct.pack("<I", 0xFEEF04BD)   # VS_FIXEDFILEINFO.dwSignature


def bump_dll(path):
    """Raise the DLL's VS_FIXEDFILEINFO file and product versions to TARGET
    when they are 14.x and lower; True if the file was changed."""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    at = data.find(FIXED_SIG)
    if at < 0:
        return False
    ms, ls = struct.unpack_from("<II", data, at + 8)
    have = (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    if have[0] != TARGET[0] or have >= TARGET:
        return False
    new_ms = (TARGET[0] << 16) | TARGET[1]
    new_ls = (TARGET[2] << 16) | TARGET[3]
    struct.pack_into("<IIII", data, at + 8, new_ms, new_ls, new_ms, new_ls)
    tmp = path + ".vcrun-tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.chmod(tmp, os.stat(path).st_mode & 0o7777)
    os.replace(tmp, path)
    return True


RUNTIMES_KEY = re.compile(r'^\[Software\\\\(?:Wow6432Node\\\\)?Microsoft\\\\VisualStudio\\\\14\.0'
                          r'\\\\VC\\\\Runtimes\\\\(?:arm64|x64|x86)\]', re.IGNORECASE)


def bump_reg(path):
    """Raise the VC\\Runtimes\\<arch> versions in a Wine .reg file; True if
    the file was changed."""
    try:
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            lines = f.readlines()
    except FileNotFoundError:
        return False
    changed = False
    i = 0
    while i < len(lines):
        if not RUNTIMES_KEY.match(lines[i]):
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not lines[j].startswith("["):
            j += 1
        values = {}
        for k in range(i + 1, j):
            m = re.match(r'^"(Major|Minor|Bld|Rbld)"=dword:([0-9a-fA-F]{8})', lines[k])
            if m:
                values[m.group(1)] = int(m.group(2), 16)
        have = (values.get("Major", 0), values.get("Minor", 0), values.get("Bld", 0), values.get("Rbld", 0))
        if have[0] == TARGET[0] and have < TARGET:
            want = dict(zip(("Major", "Minor", "Bld", "Rbld"), TARGET))
            for k in range(i + 1, j):
                m = re.match(r'^"(Major|Minor|Bld|Rbld)"=dword:', lines[k])
                if m:
                    lines[k] = '"%s"=dword:%08x\n' % (m.group(1), want[m.group(1)])
                elif lines[k].startswith('"Version"='):
                    lines[k] = '"Version"="%d.%d.%d.%d"\n' % TARGET
            changed = True
        i = j
    if changed:
        tmp = path + ".vcrun-tmp"
        with open(tmp, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.writelines(lines)
        os.replace(tmp, path)
    return changed


def main(argv):
    if len(argv) != 3 or argv[1] not in ("dist", "prefix"):
        sys.stderr.write("usage: vcrun_version.py dist DIST | prefix PFX\n")
        return 2
    where = argv[2]
    done = []
    if argv[1] == "dist":
        wine = os.path.join(where, "files", "lib", "wine")
        for arch in sorted(os.listdir(wine)) if os.path.isdir(wine) else ():
            if not arch.endswith("-windows"):
                continue
            for name in DLLS:
                p = os.path.join(wine, arch, name)
                if os.path.isfile(p) and not os.path.islink(p) and bump_dll(p):
                    done.append(os.path.join(arch, name))
        share = os.path.join(where, "files", "share")
        for d in sorted(os.listdir(share)) if os.path.isdir(share) else ():
            if d.startswith("default_pfx") and bump_reg(os.path.join(share, d, "system.reg")):
                done.append(os.path.join(d, "system.reg"))
    else:
        if bump_reg(os.path.join(where, "system.reg")):
            done.append("system.reg")
    v = "%d.%d.%d.%d" % TARGET
    for d in done:
        print("vcrun_version: %s -> %s" % (d, v))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
