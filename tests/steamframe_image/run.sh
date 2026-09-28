#!/usr/bin/env bash
# Regression test for scripts/steamframe-image.py: build small btrfs images
# the way SteamOS ships them (zstd/zlib/lzo, a default subvolume, a GPT with
# 4096-byte sectors) and check that `extract` reproduces the source tree bit
# for bit. Also rebuild the failure btrfs-progs `restore` hit on the Steam
# Frame image ("zstd frame incomplete") with frames that decode to more than
# the extent's ram_bytes, and check that `extract` still gets every byte.
#
# Runs on Linux (it needs mkfs.btrfs >= 6.11 for --rootdir --compress, and
# python3 with the `zstandard` module). Nothing is mounted; no root needed.
#   MKFS_BTRFS=/path/to/mkfs.btrfs BTRFS=/path/to/btrfs tests/steamframe_image/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
TOOL="$PWD/scripts/steamframe-image.py"
MKFS="${MKFS_BTRFS:-$(command -v mkfs.btrfs || true)}"
BTRFS="${BTRFS:-$(command -v btrfs || true)}"
[ -n "$MKFS" ] || { echo "SKIP: no mkfs.btrfs"; exit 0; }
case "$("$MKFS" --help 2>&1 || true)" in *--compress*) ;; *) echo "SKIP: $MKFS has no --compress (need btrfs-progs >= 6.11)"; exit 0 ;; esac
python3 -c 'import zstandard' 2>/dev/null || { echo "SKIP: python3 zstandard module missing"; exit 0; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { echo "  ok    $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; fail=$((fail + 1)); }

python3 - "$W/src" <<'PY'
import os, sys
root = sys.argv[1]
r = os.path.join(root, '@rootfs')
for d in ('etc', 'usr/lib', 'usr/share/deep/a/b/c'):
    os.makedirs(os.path.join(r, d), exist_ok=True)
os.makedirs(os.path.join(root, 'home'), exist_ok=True)
os.chdir(r)
open('etc/os-release', 'w').write('NAME="SteamOS"\nID=steamos\nVERSION_ID=0.3.0\n')
with open('usr/lib/big.txt', 'w') as f:
    for i in range(200000):
        f.write(f'line {i} hello steam frame arm64 {i % 97}\n')
with open('usr/lib/rand.bin', 'wb') as f:
    f.write(os.urandom(700_000))
with open('usr/lib/mixed.bin', 'wb') as f:
    for _ in range(40):
        f.write(os.urandom(4096)); f.write(b'A' * 20000)
open('etc/hostname', 'w').write('steamframe\n')
open('etc/small.conf', 'w').write('x=1\n' * 200)
os.symlink('big.txt', 'usr/lib/link.txt')
os.symlink('/usr/lib/big.txt', 'usr/lib/abslink.txt')
os.link('usr/lib/big.txt', 'usr/lib/hard.txt')
with open('usr/lib/sparse.bin', 'wb') as f:
    f.seek(5_000_000); f.write(b'end')
for i in range(300):
    open(f'usr/share/deep/a/b/c/f{i}', 'w').write(str(i) * i)
os.chmod('etc/small.conf', 0o640)
PY

same_tree() {   # expected actual: same files, bytes, links (absolute links become relative)
    python3 - "$1" "$2" <<'PY'
import os, sys, filecmp
a, b = sys.argv[1], sys.argv[2]
bad = []
for dp, dns, fns in os.walk(a):
    for n in dns + fns:
        pa = os.path.join(dp, n)
        pb = os.path.join(b, os.path.relpath(pa, a))
        if os.path.islink(pa):
            t = os.readlink(pa)
            if not os.path.islink(pb):
                bad.append(f'{pb}: not a symlink'); continue
            want = t if not t.startswith('/') else os.path.relpath(os.path.join(b, t.lstrip('/')), os.path.dirname(pb))
            if os.readlink(pb) != want:
                bad.append(f'{pb}: -> {os.readlink(pb)}, want {want}')
        elif os.path.isdir(pa):
            if not os.path.isdir(pb): bad.append(f'{pb}: missing dir')
        elif not os.path.isfile(pb) or not filecmp.cmp(pa, pb, shallow=False):
            bad.append(f'{pb}: differs')
        elif (os.stat(pa).st_mode & 0o777) != (os.stat(pb).st_mode & 0o777):
            bad.append(f'{pb}: mode {oct(os.stat(pb).st_mode)}')
for x in bad[:10]: print('   ', x)
sys.exit(1 if bad else 0)
PY
}

echo "== compression and the default subvolume"
for c in zstd:3 zlib:6 lzo; do
    n="${c%%:*}"
    truncate -s 200M "$W/$n.img"
    "$MKFS" -q -f --rootdir "$W/src" --compress "$c" -u rw:home -u default:@rootfs "$W/$n.img" >/dev/null
    if python3 "$TOOL" extract "$W/$n.img" "$W/x_$n" >"$W/log_$n" 2>&1 \
       && same_tree "$W/src/@rootfs" "$W/x_$n"; then ok "$n: extract == source"
    else bad "$n: extract differs"; tail -5 "$W/log_$n"; fi
    if python3 "$TOOL" diagnose "$W/$n.img" >"$W/diag_$n" 2>&1; then ok "$n: diagnose clean"
    else bad "$n: diagnose"; tail -8 "$W/diag_$n"; fi
done

echo "== GPT with 4096-byte sectors"
python3 - "$W/zstd.img" "$W/gpt4k.img" <<'PY'
import struct, sys, uuid, zlib
SS = 4096
root = open(sys.argv[1], 'rb').read()
parts = [('esp', 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B', 256),
         ('rootfs-A', 'B921B045-1DF0-41C3-AF44-4C6F280D3FAE', len(root) // SS),
         ('var-A', '4D21B016-B534-45C2-A9FB-5C16E091FD2D', 256)]
lba, entries, lay = 6, b'', []
for name, t, n in parts:
    entries += (uuid.UUID(t).bytes_le + uuid.uuid4().bytes_le + struct.pack('<QQQ', lba, lba + n - 1, 0)
                + name.encode('utf-16-le').ljust(72, b'\0'))
    lay.append(lba); lba += n
entries = entries.ljust(128 * 128, b'\0')
last = lba + 5
hdr = bytearray(b'EFI PART' + struct.pack('<IIIIQQQQ', 0x10000, 92, 0, 0, 1, last, 6, last - 1)
                + uuid.uuid4().bytes_le + struct.pack('<QIII', 2, 128, 128, zlib.crc32(entries)))
struct.pack_into('<I', hdr, 16, zlib.crc32(bytes(hdr[:92])))
with open(sys.argv[2], 'wb') as f:
    f.truncate((last + 1) * SS)
    f.seek(SS); f.write(hdr); f.seek(2 * SS); f.write(entries)
    f.seek(lay[1] * SS); f.write(root)
PY
python3 "$TOOL" info "$W/gpt4k.img" >"$W/info" 2>&1
grep -q 'rootfs-A  *btrfs' "$W/info" && ok "GPT/4K: rootfs-A found as btrfs" || { bad "GPT/4K info"; cat "$W/info"; }
python3 "$TOOL" extract "$W/gpt4k.img" "$W/x_gpt" >/dev/null 2>&1 && same_tree "$W/src/@rootfs" "$W/x_gpt" \
    && ok "GPT/4K: extract == source" || bad "GPT/4K extract"

echo "== frames larger than ram_bytes (btrfs-progs: \"zstd frame incomplete\")"
cp "$W/zstd.img" "$W/big.img"
python3 - "$TOOL" "$W/big.img" <<'PY'
import importlib.util, sys, zstandard
spec = importlib.util.spec_from_file_location('sf', sys.argv[1])
sf = importlib.util.module_from_spec(spec); spec.loader.exec_module(sf)
img = sf.Image(sys.argv[2]); fs = sf.Btrfs(img, 0, img.size)
fs.roots()
inodes, _ = fs.load_tree(fs.default_subvol)
done = 0
with open(sys.argv[2], 'r+b') as f:
    for rec in inodes.values():
        for e in rec['ext']:
            if done < 3 and e['type'] == 1 and e['comp'] == 3 and e['ram'] == 131072:
                data, _ = sf.zstd_decode(fs.read_logical(e['disk'], e['disk_len']), e['ram'])
                frame = zstandard.ZstdCompressor(level=19).compress(data + b'\0' * 16384)
                if len(frame) <= e['disk_len']:
                    f.seek(fs.phys(e['disk'])); f.write(frame.ljust(e['disk_len'], b'\0')); done += 1
sys.exit(0 if done == 3 else 1)
PY
if [ -n "$BTRFS" ]; then
    mkdir -p "$W/r"
    # -s: the file is in a subvolume, which restore skips otherwise.
    out="$("$BTRFS" restore -s "$W/big.img" "$W/r" 2>&1 || true)"
    case "$out" in
        *'zstd frame incomplete'*) ok "btrfs restore fails with \"zstd frame incomplete\" (reproduced)" ;;
        *) bad "btrfs restore did not report the frame error"; echo "$out" | tail -3 ;;
    esac
fi
python3 "$TOOL" extract "$W/big.img" "$W/x_big" >"$W/log_big" 2>&1 && same_tree "$W/src/@rootfs" "$W/x_big" \
    && ok "extract == source despite the larger frames" || { bad "extract with larger frames"; tail -4 "$W/log_big"; }
python3 "$TOOL" diagnose "$W/big.img" 2>/dev/null | grep -q 'frame-larger-than-ram  *3' \
    && ok "diagnose names the 3 larger frames" || bad "diagnose count"

echo "== inventory"
python3 "$TOOL" inventory "$W/x_zstd" --json "$W/inv.json" --md "$W/inv.md" >/dev/null
python3 -c "import json,sys; j=json.load(open(sys.argv[1])); sys.exit(0 if j['os_release'].get('ID')=='steamos' else 1)" "$W/inv.json" \
    && ok "inventory reads os-release" || bad "inventory"

echo "== pacman repositories and compare"
mkdir -p "$W/x_zstd/etc/pacman.d"
printf '[options]\nArchitecture = auto\n[holo-core-aarch64-preview]\nInclude = /etc/pacman.d/m\n' > "$W/x_zstd/etc/pacman.conf"
echo 'Server = https://mirror.invalid/$repo/os/$arch' > "$W/x_zstd/etc/pacman.d/m"
mkdir -p "$W/x_zstd/usr/lib/holo/pacmandb/local/nss-3.117-1"
printf '%%NAME%%\nnss\n\n%%VERSION%%\n3.117-1\n\n%%ARCH%%\naarch64\n' > "$W/x_zstd/usr/lib/holo/pacmandb/local/nss-3.117-1/desc"
python3 "$TOOL" inventory "$W/x_zstd" --json "$W/inv2.json" --md "$W/inv2.md" >/dev/null
grep -q 'mirror.invalid/holo-core-aarch64-preview/os/aarch64' "$W/inv2.md" && ok "inventory lists pacman repositories" || bad "pacman repositories"
python3 - "$W/repo.db" <<'PY2'
import io, sys, tarfile, zstandard
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w') as tf:
    for n, v in (('nss', '3.118-1'), ('glibc', '2.42-1')):
        d = f'%NAME%\n{n}\n\n%VERSION%\n{v}\n\n%ARCH%\naarch64\n'.encode()
        ti = tarfile.TarInfo(f'{n}-{v}/desc'); ti.size = len(d); tf.addfile(ti, io.BytesIO(d))
open(sys.argv[1], 'wb').write(zstandard.ZstdCompressor().compress(buf.getvalue()))
PY2
out="$(python3 "$TOOL" compare "$W/inv2.json" "$W/repo.db")"
case "$out" in *'| nss | 3.117-1 | 3.118-1 |'*'differs'*) ok "compare finds the version difference" ;; *) bad "compare"; echo "$out" | tail -4 ;; esac

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
