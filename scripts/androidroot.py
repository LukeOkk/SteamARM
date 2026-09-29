#!/usr/bin/env python3
"""Helpers for the Android root scripts/mkandroidroot.sh builds (no VM).

  androidroot.py image-kind <img>           sparse | ext4 | erofs | unknown
  androidroot.py unsparse <in> <out>        Android sparse image -> raw (simg2img)
  androidroot.py apex-names <root>          print "<dir> <manifest name> <version>"
                                            for each /system/apex/<dir>
  androidroot.py inventory <root> [--lxrun build/lxrun] [--x18-limit N]
                                            ELF census of the root: machine and
                                            class, type, PIE, PT_LOAD p_align,
                                            PT_TLS, and x18 sites counted by
                                            `lxrun --dry-run` (which loads and
                                            rewrites, and never runs the guest)

Pure Python, reads files only. Nothing here writes into the root.
"""
import collections
import os
import re
import struct
import subprocess
import sys

EM = {3: "i386", 8: "mips", 40: "arm", 62: "x86-64", 183: "aarch64", 243: "riscv"}
PT_LOAD, PT_INTERP, PT_TLS = 1, 3, 7


def apex_manifest(path):
    """name (field 1) and version (field 2) of an apex_manifest.pb."""
    b = open(path, "rb").read()
    i, name, ver = 0, None, None

    def varint(i):
        r = s = 0
        while True:
            c = b[i]
            i += 1
            r |= (c & 0x7F) << s
            s += 7
            if c < 0x80:
                return r, i

    while i < len(b):
        key, i = varint(i)
        field, wt = key >> 3, key & 7
        if wt == 2:
            n, i = varint(i)
            v = b[i:i + n]
            i += n
            if field == 1:
                name = v.decode()
        elif wt == 0:
            v, i = varint(i)
            if field == 2:
                ver = v
        elif wt == 1:
            i += 8
        elif wt == 5:
            i += 4
        else:
            break
    return name, ver


SPARSE_MAGIC = 0xED26FF3A


def image_kind(path):
    """'sparse', 'ext4', 'erofs' or 'unknown', from the magic numbers."""
    with open(path, "rb") as f:
        head = f.read(2048)
    if len(head) >= 4 and struct.unpack_from("<I", head, 0)[0] == SPARSE_MAGIC:
        return "sparse"
    if len(head) >= 1082 and head[1080:1082] == b"\x53\xef":
        return "ext4"
    if len(head) >= 1028 and struct.unpack_from("<I", head, 1024)[0] == 0xE0F5E1E2:
        return "erofs"
    return "unknown"


def cmd_unsparse(src, dst):
    """Android sparse image -> raw image (what simg2img does)."""
    with open(src, "rb") as f, open(dst, "wb") as o:
        magic, major, minor, fhs, chs, blk, total_blks, total_chunks, _ = struct.unpack("<IHHHHIIII", f.read(28))
        if magic != SPARSE_MAGIC or major != 1:
            raise SystemExit("%s: not an Android sparse image v1" % src)
        f.seek(fhs)
        for _ in range(total_chunks):
            ctype, _, csz, tsz = struct.unpack("<HHII", f.read(12))
            f.seek(chs - 12, 1)
            n = csz * blk
            if ctype == 0xCAC1:            # raw
                left = n
                while left:
                    buf = f.read(min(left, 1 << 22))
                    if not buf:
                        raise SystemExit("%s: truncated raw chunk" % src)
                    o.write(buf)
                    left -= len(buf)
            elif ctype == 0xCAC2:          # fill
                word = f.read(4)
                o.write(word * (n // 4))
            elif ctype == 0xCAC3:          # don't care
                o.seek(n, 1)
            elif ctype == 0xCAC4:          # crc32
                f.read(4)
            else:
                raise SystemExit("%s: unknown chunk type 0x%x" % (src, ctype))
        o.truncate(total_blks * blk)


def cmd_apex_names(root):
    base = os.path.join(root, "system", "apex")
    for d in sorted(os.listdir(base)):
        m = os.path.join(base, d, "apex_manifest.pb")
        if os.path.isfile(m):
            name, ver = apex_manifest(m)
            print(d, name or d, ver if ver is not None else "?")
        elif d.endswith((".apex", ".capex")):
            # A compressed/zip APEX: not flattened on this image. The caller
            # extracts apex_payload.img itself.
            print(d, "ZIP", "?")


def elf_info(path):
    try:
        with open(path, "rb") as f:
            h = f.read(64)
            if len(h) < 52 or h[:4] != b"\x7fELF":
                return None
            cls, data = h[4], h[5]
            if data != 1:
                return {"class": cls, "machine": "big-endian", "type": 0}
            if cls == 2:
                e_type, e_machine = struct.unpack_from("<HH", h, 16)
                e_phoff, = struct.unpack_from("<Q", h, 32)
                e_phentsize, e_phnum = struct.unpack_from("<HH", h, 54)
                f.seek(e_phoff)
                ph = f.read(e_phentsize * e_phnum)
                phdrs = [struct.unpack_from("<IIQQQQQQ", ph, k * e_phentsize) for k in range(e_phnum)]
                loads = [p for p in phdrs if p[0] == PT_LOAD]
                return {
                    "class": 64, "machine": EM.get(e_machine, str(e_machine)), "type": e_type,
                    "interp": any(p[0] == PT_INTERP for p in phdrs),
                    "tls": any(p[0] == PT_TLS for p in phdrs),
                    "align": min((p[7] for p in loads), default=0),
                    "exec_load": any(p[1] & 1 for p in loads),
                }
            e_type, e_machine = struct.unpack_from("<HH", h, 16)
            return {"class": 32, "machine": EM.get(e_machine, str(e_machine)), "type": e_type}
    except OSError:
        return None


X18_RE = re.compile(r"x18 (\d+) found in (\d+) code windows, (\d+) rewritten, (\d+) unsupported, (\d+) unreachable")


def x18_count(lxrun, path, root):
    """The first image's x18 line of a dry run (the program itself, not its
    interpreter), or None when lxrun refused the file."""
    env = dict(os.environ, LXRT_ROOT=root, LXRT_GUEST_PAGE="4096")
    try:
        out = subprocess.run([lxrun, "--dry-run", path], env=env, capture_output=True,
                             text=True, timeout=60).stderr
    except subprocess.TimeoutExpired:
        return None
    m = X18_RE.search(out)
    if not m:
        return None
    return tuple(int(x) for x in m.groups())


def cmd_inventory(root, lxrun, x18_limit):
    root = os.path.realpath(root)
    files = []
    for top in ("system", "vendor", "apex"):
        base = os.path.join(root, top)
        for dp, dn, fn in os.walk(base):
            for n in fn:
                p = os.path.join(dp, n)
                if os.path.islink(p) or not os.path.isfile(p):
                    continue
                files.append(p)
    by_machine = collections.Counter()
    aligns = collections.Counter()
    kinds = collections.Counter()
    tls = 0
    elfs64 = []
    for p in files:
        e = elf_info(p)
        if not e:
            continue
        by_machine[(e["class"], e["machine"])] += 1
        if e["class"] != 64 or e["machine"] != "aarch64":
            continue
        rel = os.path.relpath(p, root)
        kind = {2: "EXEC", 3: "DYN"}.get(e["type"], "type%d" % e["type"])
        if e["type"] == 3 and e["interp"]:
            kind = "DYN+INTERP (PIE executable)"
        kinds[kind] += 1
        aligns[hex(e["align"])] += 1
        tls += e["tls"]
        elfs64.append((rel, e))
    # /apex is a copy of /system/apex here: say so rather than count twice.
    print("root: %s" % root)
    print("files scanned (regular, not symlinks) under system/ vendor/ apex/: %d" % len(files))
    print("ELF by class/machine:")
    for (c, m), n in sorted(by_machine.items(), key=lambda kv: -kv[1]):
        print("  ELF%d %-8s %6d" % (c, m, n))
    print("ELF64 aarch64 by type:")
    for k, n in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print("  %-28s %6d" % (k, n))
    print("ELF64 aarch64 by smallest PT_LOAD p_align:")
    for a, n in sorted(aligns.items(), key=lambda kv: int(kv[0], 16)):
        print("  %-8s %6d" % (a, n))
    print("ELF64 aarch64 with PT_TLS: %d" % tls)
    for rel in ("system/apex/com.android.runtime/bin/linker64",
                "system/apex/com.android.runtime/lib64/bionic/libc.so",
                "system/bin/bootstrap/linker64", "system/lib64/bootstrap/libc.so",
                "system/bin/toybox", "system/bin/sh", "system/bin/dalvikvm64",
                "system/apex/com.android.art.release/lib64/libart.so"):
        p = os.path.join(root, rel)
        e = elf_info(p) if os.path.isfile(p) else None
        if e:
            print("  %-58s %s %s p_align %s%s" % (rel, e["machine"], {2: "EXEC", 3: "DYN"}.get(e["type"]),
                                                  hex(e["align"]), " PT_INTERP" if e["interp"] else ""))
    if lxrun:
        # x18 over the aarch64 ELF files that hold code, /system/apex and
        # /vendor included; /apex (a copy) skipped.
        cand = [(r, e) for r, e in elfs64 if e["exec_load"] and not r.startswith("apex/")]
        if x18_limit:
            cand = cand[:x18_limit]
        tot = [0, 0, 0, 0]
        files_with = 0
        refused = []
        top = []
        for rel, e in cand:
            c = x18_count(lxrun, os.path.join(root, rel), root)
            if c is None:
                refused.append(rel)
                continue
            found, _, rewritten, unsup, unreach = c
            tot[0] += found
            tot[1] += rewritten
            tot[2] += unsup
            tot[3] += unreach
            if found:
                files_with += 1
                top.append((found, rel, unsup, unreach))
        print("x18 (lxrun --dry-run, LXRT_GUEST_PAGE=4096) over %d aarch64 ELF files with code:" % len(cand))
        print("  files with x18 sites: %d; sites found %d, rewritten %d, unsupported %d, unreachable %d"
              % (files_with, tot[0], tot[1], tot[2], tot[3]))
        print("  refused by lxrun (no x18 line): %d%s" % (len(refused), (": " + ", ".join(refused[:10])) if refused else ""))
        for found, rel, unsup, unreach in sorted(top, reverse=True)[:15]:
            print("  %6d  %s%s" % (found, rel, (" (unsupported %d, unreachable %d)" % (unsup, unreach)) if unsup or unreach else ""))


def main(argv):
    if len(argv) >= 2 and argv[0] == "image-kind":
        print(image_kind(argv[1]))
        return 0
    if len(argv) >= 3 and argv[0] == "unsparse":
        cmd_unsparse(argv[1], argv[2])
        return 0
    if len(argv) >= 2 and argv[0] == "apex-names":
        cmd_apex_names(argv[1])
        return 0
    if len(argv) >= 2 and argv[0] == "inventory":
        lxrun = None
        limit = 0
        a = argv[2:]
        while a:
            if a[0] == "--lxrun" and len(a) > 1:
                lxrun = a[1]
                a = a[2:]
            elif a[0] == "--x18-limit" and len(a) > 1:
                limit = int(a[1])
                a = a[2:]
            else:
                print("unknown option %s" % a[0], file=sys.stderr)
                return 2
        cmd_inventory(argv[1], lxrun, limit)
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
