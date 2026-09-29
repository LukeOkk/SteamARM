#!/usr/bin/env python3
"""Retarget Heroic's app.asar from its linux-x64 helpers to linux-arm64 ones.

    scripts/heroic-asar.py retarget IN.asar OUT.asar ARM64-BIN-DIR
    scripts/heroic-asar.py list IN.asar [PREFIX]

Heroic's app.asar holds no native Node module: every file in it is the same
for x64 and arm64 except its per-architecture helpers (legendary, gogdl,
nile, comet, vulkan-helper), which electron-builder puts in build/bin/<arch>/
linux and leaves unpacked beside the archive (app.asar.unpacked). MEASURED
(benchmarks/stage24-heroic.txt): Heroic 2.22.3 built from its source tag with
`electron-builder --linux dir --arm64` has 1853 files in its app.asar; 1848
are byte-identical to the official linux-x64 release's, and the 5 others are
exactly those helpers.

An unpacked file has an entry in the archive's header (size, "unpacked",
integrity) and no data in it, and the data offsets count from the end of the
header. So "retarget" rewrites the header only: build/bin/x64/linux goes, and
build/bin/arm64/linux gets an entry for each file of ARM64-BIN-DIR, placed and
serialised as electron-builder does; the data section is copied unchanged.
Heroic finds its helpers at build/bin/<process.arch>/<platform> through the
archive (archSpecificBinary, main.js), so the entries must be there, not just
the files. The header format: a Chromium pickle holding the JSON (UInt32
payload size, UInt32 string length, the UTF-8 string, padding to 4), behind
an 8-byte prefix (UInt32 4, UInt32 header pickle size).
"""
import hashlib, json, os, struct, sys

BLOCK = 4 * 1024 * 1024          # electron-builder's integrity block size


def read_asar(path):
    with open(path, "rb") as f:
        four, hsize = struct.unpack("<II", f.read(8))
        if four != 4:
            raise SystemExit("%s: not an asar archive" % path)
        pickle = f.read(hsize)
        payload, slen = struct.unpack("<II", pickle[:8])
        header = json.loads(pickle[8:8 + slen].decode("utf-8"))
    return header, 8 + hsize


def integrity(path):
    whole = hashlib.sha256()
    blocks = []
    with open(path, "rb") as f:
        while True:
            b = f.read(BLOCK)
            if not b:
                break
            whole.update(b)
            blocks.append(hashlib.sha256(b).hexdigest())
    if not blocks:
        blocks.append(hashlib.sha256(b"").hexdigest())
    return {"algorithm": "SHA256", "hash": whole.hexdigest(), "blockSize": BLOCK, "blocks": blocks}


def encode(header):
    s = json.dumps(header, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    pad = (4 - len(s) % 4) % 4
    pickle = struct.pack("<II", 4 + len(s) + pad, len(s)) + s + b"\0" * pad
    return struct.pack("<II", 4, len(pickle)) + pickle


def retarget(src, dst, bindir):
    header, data_at = read_asar(src)
    bins = header["files"]["build"]["files"]["bin"]["files"]
    x64 = bins.get("x64", {}).get("files", {})
    if "linux" not in x64:
        raise SystemExit("%s: no build/bin/x64/linux in the archive" % src)
    if "arm64" in bins:
        raise SystemExit("%s: already has build/bin/arm64" % src)
    names = sorted(n for n in os.listdir(bindir) if not n.startswith("."))
    if not names:
        raise SystemExit("%s: no helpers" % bindir)
    linux = {}
    for n in names:
        p = os.path.join(bindir, n)
        linux[n] = {"size": os.path.getsize(p), "unpacked": True, "integrity": integrity(p)}
    del x64["linux"]
    # Key order as electron-builder writes it: legendary.LICENSE, arm64, x64.
    new = {}
    for k, v in bins.items():
        if k == "x64":
            new["arm64"] = {"files": {"linux": {"files": linux}}}
            if v["files"]:
                new["x64"] = v
        else:
            new[k] = v
    if "arm64" not in new:
        new["arm64"] = {"files": {"linux": {"files": linux}}}
    header["files"]["build"]["files"]["bin"]["files"] = new
    tmp = dst + ".tmp"
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        fo.write(encode(header))
        fi.seek(data_at)
        while True:
            b = fi.read(1 << 20)
            if not b:
                break
            fo.write(b)
    os.replace(tmp, dst)
    print("retargeted %s -> %s: build/bin/arm64/linux = %s" % (src, dst, " ".join(names)))


def listing(src, prefix=""):
    header, _ = read_asar(src)

    def walk(node, path):
        for k, v in node.get("files", {}).items():
            p = path + "/" + k if path else k
            if "files" in v:
                yield from walk(v, p)
            else:
                yield p, v

    for p, v in walk(header, ""):
        if p.startswith(prefix):
            print("%s %d%s" % (p, v.get("size", 0), " unpacked" if v.get("unpacked") else ""))


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "retarget":
        retarget(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) in (3, 4) and sys.argv[1] == "list":
        listing(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else "")
    else:
        sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
