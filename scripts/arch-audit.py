#!/usr/bin/env python3
"""Architecture audit: what each part of SteamARM really is, read from the
files and the processes, never from what it is meant to be.

  scripts/arch-audit.py                 the installed stack and the processes now
  scripts/arch-audit.py FILE...         those files only

A Windows PE is x86, x64, ARM64, ARM64EC (x64 machine type with CHPE
metadata: ARM64 code that keeps the x64 ABI, what Wine ARM64EC and the
Proton ARM64 DXVK/vkd3d-proton are) or ARM64X (both), and whether its code
carries volatile metadata (FEX's ARM64EC JIT drops TSO barriers on the
accesses it marks as not volatile). An ELF is x86-64, i386 or aarch64. A
Mach-O is arm64, x86_64 or both. A process is native, translated by Rosetta
(the kernel's P_TRANSLATED flag) or, for SteamARM's runtime, the guest it
runs: an aarch64 ELF directly, or x86 through FEX.
"""
import ctypes
import glob
import os
import struct
import subprocess
import sys

MACHINES = {0x14C: "x86", 0x8664: "x64", 0xAA64: "ARM64", 0xA641: "ARM64EC", 0xA64E: "ARM64X"}
ELF = {0x03: "i386", 0x3E: "x86-64", 0xB7: "aarch64"}


def pe(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    machine, nsect = struct.unpack_from("<HH", data, pe_off + 4)
    opt = pe_off + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    plus = magic == 0x20B
    ndirs_off = opt + (108 if plus else 92)
    dirs_off = opt + (112 if plus else 96)
    ndirs = struct.unpack_from("<I", data, ndirs_off)[0]
    sects = []
    sect_off = opt + struct.unpack_from("<H", data, pe_off + 20)[0]
    for i in range(nsect):
        name, vsize, va, rawsize, raw = struct.unpack_from("<8sIIII", data, sect_off + 40 * i)
        sects.append((va, vsize, raw, rawsize))

    def file_off(rva):
        for va, vsize, raw, rawsize in sects:
            if va <= rva < va + max(vsize, rawsize):
                return raw + rva - va
        return None

    kind = MACHINES.get(machine, "machine %#x" % machine)
    chpe = volatile = 0
    if ndirs > 10 and plus:
        rva, size = struct.unpack_from("<II", data, dirs_off + 8 * 10)
        off = file_off(rva) if rva else None
        if off is not None and size:
            lc_size = struct.unpack_from("<I", data, off)[0]
            if lc_size >= 0xC8 + 8:
                chpe = struct.unpack_from("<Q", data, off + 0xC8)[0]
            if lc_size >= 0x108 + 8:
                volatile = struct.unpack_from("<Q", data, off + 0x108)[0]
    if chpe:
        kind = "ARM64X" if machine == 0xAA64 else "ARM64EC" if machine == 0x8664 else kind + "+CHPE"
    notes = []
    if kind in ("x64", "x86"):
        notes.append("volatile metadata" if volatile else "no volatile metadata")
    return "PE %s%s" % (kind, " (" + ", ".join(notes) + ")" if notes else "")


def elf(path):
    with open(path, "rb") as f:
        head = f.read(20)
    if head[:4] != b"\x7fELF":
        return None
    machine = struct.unpack_from("<H", head, 18)[0]
    return "ELF %s" % ELF.get(machine, "machine %#x" % machine)


def macho(path):
    with open(path, "rb") as f:
        head = f.read(8)
    magic = head[:4]
    if magic == b"\xcf\xfa\xed\xfe":
        cpu = struct.unpack_from("<I", head, 4)[0]
        return "Mach-O %s" % {0x0100000C: "arm64", 0x01000007: "x86_64"}.get(cpu, "cpu %#x" % cpu)
    if magic in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
        out = subprocess.run(["lipo", "-archs", path], capture_output=True, text=True).stdout.strip()
        return "Mach-O universal (%s)" % out
    return None


def classify(path):
    try:
        for probe in (pe, elf, macho):
            r = probe(path)
            if r:
                return r
    except (OSError, struct.error) as e:
        return "unreadable (%s)" % e.__class__.__name__
    return "not a program"


# ------------------------------------------------------------------ processes

P_TRANSLATED = 0x00020000


def translated(pid):
    """The kernel's P_TRANSLATED flag of a process (Rosetta), or None."""
    libc = ctypes.CDLL(None, use_errno=True)
    mib = (ctypes.c_int * 4)(1, 14, 1, pid)      # CTL_KERN, KERN_PROC, KERN_PROC_PID
    size = ctypes.c_size_t(0)
    if libc.sysctl(mib, 4, None, ctypes.byref(size), None, 0) != 0 or size.value < 40:
        return None
    buf = ctypes.create_string_buffer(size.value)
    if libc.sysctl(mib, 4, buf, ctypes.byref(size), None, 0) != 0:
        return None
    flag = struct.unpack_from("<i", buf.raw, 32)[0]      # extern_proc.p_flag
    return bool(flag & P_TRANSLATED)


def processes():
    out = subprocess.run(["ps", "-axwwo", "pid=,command="], capture_output=True, text=True).stdout
    rosetta, guests = [], {"native aarch64": 0, "x86 through FEX": 0}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        pid, _, cmd = line.partition(" ")
        pid = int(pid)
        if cmd.startswith("arch -x86_64") or translated(pid):
            rosetta.append(cmd[:80])
        words = cmd.split()
        if words and words[0].endswith("/lxrun") and len(words) > 1:
            if "/FEX" in words[1] or words[1].endswith("FEX-gb"):
                guests["x86 through FEX"] += 1
            else:
                guests["native aarch64"] += 1
    return rosetta, guests


# ------------------------------------------------------------------ the stack

def home(p):
    return os.path.expanduser(p)


def stack():
    state = os.environ.get("STEAMARM_STATE") or home("~/SteamARM-roots")
    build = os.environ.get("STEAMARM_BUILD") or home("~/SteamARM-build")
    x86 = state + "/steamroot/tmp/fexhome/.local/share/Steam/steamapps/common"
    frame = state + "/arm64root/tmp/armhome/.local/share/Steam/steamapps/common"
    items = [
        ("SteamARM.app", "/Applications/SteamARM.app/Contents/MacOS/SteamARM"),
        ("runtime lxrun", home("~/Library/Application Support/SteamARM/src/build/lxrun")),
        ("X server", build + "/xquartz/SteamARM-X11.app/Contents/MacOS/X11.bin"),
        ("KosmicKrisp", build + "/mesa-kk/out/libvulkan_kosmickrisp.dylib"),
        ("MoltenVK", (glob.glob(build + "/moltenvk/**/libMoltenVK.dylib", recursive=True) or [""])[0]),
        ("FEX (host, translates x86 guests)", state + "/steamroot/usr/lib/lxrt-emu/FEX"),
        ("Vulkan shim (in the x86 root)", state + "/steamroot/usr/lib/lxrt-emu/libvulkan.so.1"),
        ("Steam client (SteamARM)", state + "/arm64root/tmp/armhome/.local/share/Steam/steamrtarm64/steam"),
        ("x86 Proton: wineserver", x86 + "/Proton - Experimental/files/bin/wineserver"),
        ("x86 Proton: wine (unix side)", x86 + "/Proton - Experimental/files/bin/wine"),
        ("x86 Proton: ntdll.so (unix)", x86 + "/Proton - Experimental/files/lib/wine/x86_64-unix/ntdll.so"),
        ("x86 Proton: ntdll.dll", x86 + "/Proton - Experimental/files/lib/wine/x86_64-windows/ntdll.dll"),
        ("x86 Proton: kernel32.dll", x86 + "/Proton - Experimental/files/lib/wine/x86_64-windows/kernel32.dll"),
        ("x86 Proton: win32u.dll", x86 + "/Proton - Experimental/files/lib/wine/x86_64-windows/win32u.dll"),
        ("x86 Proton: DXVK d3d11.dll", x86 + "/Proton - Experimental/files/lib/wine/dxvk/x86_64-windows/d3d11.dll"),
        ("x86 Proton: vkd3d-proton d3d12.dll", x86 + "/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll"),
    ]
    arm = None
    for d in (frame, x86):
        for name in ("Proton Experimental (ARM64)", "Proton 11.0 (ARM64)"):
            if os.path.exists(d + "/" + name + "/files/lib/wine/aarch64-windows/ntdll.dll"):
                arm = d + "/" + name
                break
        if arm:
            break
    if arm:
        w = arm + "/files/lib/wine"
        items += [
            ("ARM64 Proton (Valve, not usable on macOS yet): wineserver", arm + "/files/bin-arm64/wineserver"),
            ("ARM64 Proton: wine (loader)", arm + "/files/bin-arm64/wine"),
            ("ARM64 Proton: ntdll.so (unix)", w + "/aarch64-unix/ntdll.so"),
            ("ARM64 Proton: ntdll.dll", w + "/aarch64-windows/ntdll.dll"),
            ("ARM64 Proton: kernel32.dll", w + "/aarch64-windows/kernel32.dll"),
            ("ARM64 Proton: win32u.dll", w + "/aarch64-windows/win32u.dll"),
            ("ARM64 Proton: FEX ARM64EC (x64 code in-process)", w + "/aarch64-windows/libarm64ecfex.dll"),
            ("ARM64 Proton: FEX WoW64 (x86 code)", w + "/aarch64-windows/libwow64fex.dll"),
            ("ARM64 Proton: xtajit64.dll", w + "/aarch64-windows/xtajit64.dll"),
            ("ARM64 Proton: DXVK d3d11.dll", w + "/dxvk/aarch64-windows/d3d11.dll"),
            ("ARM64 Proton: DXVK dxgi.dll", w + "/dxvk/aarch64-windows/dxgi.dll"),
            ("ARM64 Proton: vkd3d-proton d3d12.dll", w + "/vkd3d-proton/aarch64-windows/d3d12.dll"),
            ("ARM64 Proton: x64 ntdll.dll (for x64 programs)", w + "/x86_64-windows/ntdll.dll"),
        ]
    game = frame + "/Schedule I"
    if os.path.isdir(game):
        items += [
            ("Schedule I: Schedule I.exe", game + "/Schedule I.exe"),
            ("Schedule I: UnityPlayer.dll", game + "/UnityPlayer.dll"),
            ("Schedule I: GameAssembly.dll", game + "/GameAssembly.dll"),
        ]
    return items


def main(argv):
    if argv:
        for p in argv:
            print("%s: %s" % (p, classify(p)))
        return 0
    for label, path in stack():
        print("%-58s %s" % (label + ":", classify(path) if path and os.path.exists(path) else "not installed"))
    rosetta, guests = processes()
    print()
    print("Rosetta (translated) processes: %d" % len(rosetta))
    for c in rosetta:
        print("   " + c)
    print("runtime guests now: %s" % ", ".join("%s %d" % (k, v) for k, v in guests.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
