#!/usr/bin/env bash
# Build the aarch64 guest root (LXRT_ROOT) on the macOS host from Fedora 43
# aarch64 RPMs, with no VM anywhere.
#
# Why this exists: exit criterion #8 -- every component must build without
# the Fedora VM. tests/elf/mkroot.sh and scripts/mkaarch64extras.sh (retired
# with the VM, no longer in the project) made this root inside the VM (dnf install, gcc, ldd, patchelf) and copied it out over
# ssh. This is their VM-free replacement; it produces the same tree:
#
#   usr/bin        bash (+ sh -> bash), Xvnc, xkbcomp, and FEX from
#                  scripts/build-fex-host.sh: FEX-gb, FEX-gb-dbg, FEXServer,
#                  FEXServer-dbg, FEXGetConfig
#   usr/lib64      every library those programs, Mesa's swrast and FEX need
#                  (the ldd closure, flattened to real files named by soname),
#                  libgallium-*.so, dri/{swrast_dri.so,libdril_dri.so},
#                  ld-linux-aarch64.so.1, and the runtime's libvulkan.so.1
#                  shim if `make shim` has built it
#   usr/lib        ld-linux-aarch64.so.1; lxrt-emu/ = FEX-emu as FEX plus
#                  ld.so, libc, libm, libstdc++, libgcc_s (the emulator prefix)
#   usr/share/X11/xkb   xkeyboard-config's data, symlinks resolved
#   etc/{resolv.conf,hosts}, var/lib/xkb, dev/shm, tmp,
#   bin lib lib64 sbin -> usr/... (merged /usr, as on Fedora)
#
# Steps:
#   lock     only with --relock (or without scripts/mkroot-rpm.lock): read
#            today's Fedora 43 repodata (releases/ and updates/, primary.xml.zst,
#            checked against the sha256 in repomd.xml; updates/ wins), start
#            from the seed packages below and add, until nothing changes, the
#            package that provides every shared library ("libfoo.so.N()(64bit)")
#            one of them requires. A package another one obsoletes is replaced
#            by it, as dnf does on an upgrade (F43's tigervnc 1.16 turned
#            tigervnc-server-minimal into tigervnc-x11-server). The result --
#            file names, epochs, sha256, sizes, source RPMs, and why each is
#            there -- goes to scripts/mkroot-rpm.lock. A normal run never reads
#            repodata: the lock is the input, so the build is reproducible.
#   fetch    download every locked RPM over HTTPS: updates/, releases/, the
#            archive, then Koji (which keeps every build), and check its sha256.
#            Cache shared with scripts/build-fex-host.sh ($STEAMARM_BUILD/rpms).
#   stage    unpack them all with bsdtar into $STEAMARM_BUILD/rootstage-f43
#            (merged /usr, absolute symlinks made relative). No rpm, no dnf,
#            no scriptlets (see SCRIPTLETS).
#   root     assemble <out-dir> from the stage and $STEAMARM_BUILD/out.
#   samples  only with a samples dir: compile the test programs of
#            tests/elf/mkroot.sh (hello_glibc, hello_dyn, tls_test, ...) with
#            Homebrew clang + lld against the stage, which also holds the
#            lock's "build" packages (glibc-devel, glibc-static,
#            kernel-headers, gcc's crt files and libgcc.a, vulkan-headers).
#   check    every DT_NEEDED of every ELF in <out-dir> must resolve inside it
#            (the emulator prefix only inside usr/lib/lxrt-emu).
#
# SCRIPTLETS: none are run, and none are needed here. Of the root packages
# only six have any (read from their RPM headers): glibc (ldconfig,
# iconvconfig, ld.so.conf; a kernel-version check), bash (/etc/shells),
# tigervnc-x11-server (systemd units), glib2 (gio module and schema caches),
# freetype (touches fonts.scale), xkeyboard-config (moves an old xkb dir
# aside). The VM root had no ld.so.cache or gconv cache either: ld.so falls
# back to /lib64:/usr/lib64, and the runtime never reads the cache.
# /usr/bin/sh is a plain symlink in Fedora's bash payload (no alternatives).
# Fedora's /usr/share/X11/xkb -> xkeyboard-config-2 link is copied resolved,
# as the VM script's `cp -aL` did.
#
# Usage: scripts/mkroot-rpm.sh [--relock] <out-dir> [samples-dir]
#        scripts/mkroot-rpm.sh --relock           (only rewrite the lock)
#   <out-dir>      must not exist, or be an earlier output of this script
#                  (it is then rebuilt; its tmp/ -- runtime state -- is kept)
#   [samples-dir]  where the test programs go (tests/elf/run.sh reads
#                  LXRT_SAMPLES; the VM script used /tmp/lxrt-samples)
#
# Environment:
#   STEAMARM_BUILD  work directory     (default $HOME/SteamARM-build)
#   FEX_OUT         FEX binaries       (default $STEAMARM_BUILD/out, from
#                                       scripts/build-fex-host.sh)
#   LLVM_BIN, LLD   as in scripts/build-fex-host.sh (samples only)
#
# Needs: curl, bsdtar (macOS tar), zstd (Homebrew: bsdtar hands Fedora's zstd
# payloads to it), python3, shasum; for samples Homebrew llvm + lld.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
RPMDIR="$WORK/rpms"
META="$WORK/repodata-f43"
STAGE="${MKROOT_STAGE:-$WORK/rootstage-f43}"
FEX_OUT="${FEX_OUT:-$WORK/out}"
LOCK="${MKROOT_LOCK:-$REPO/scripts/mkroot-rpm.lock}"
LLVM_BIN="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
if [ -z "${LLD:-}" ]; then
    if [ -x "$LLVM_BIN/ld.lld" ]; then LLD="$LLVM_BIN/ld.lld"; else LLD="$(command -v ld.lld || true)"; fi
fi

F43_UPDATES=https://dl.fedoraproject.org/pub/fedora/linux/updates/43/Everything/aarch64
F43_RELEASE=https://dl.fedoraproject.org/pub/fedora/linux/releases/43/Everything/aarch64/os
F43_ARCHIVE_UPDATES=https://dl.fedoraproject.org/pub/archive/fedora/linux/updates/43/Everything/aarch64
F43_ARCHIVE_RELEASE=https://dl.fedoraproject.org/pub/archive/fedora/linux/releases/43/Everything/aarch64/os
KOJI=https://kojipkgs.fedoraproject.org/packages

# What the VM scripts installed with dnf (mkaarch64extras.sh: tigervnc-server-
# minimal, xkbcomp; mkroot.sh: the glibc runtime) plus what they took from
# the VM's base system (bash, xkeyboard-config, mesa-dri-drivers, libstdc++
# and libgcc for FEX). Shared-library dependencies are added by the lock step.
ROOT_SEEDS=(glibc libgcc libstdc++ bash tigervnc-server-minimal xkbcomp xkeyboard-config mesa-dri-drivers)
# Another root (scripts/mkarmroot.sh): its own seeds; "so:libfoo.so.N" seeds
# the package that provides that shared library.
[ -n "${MKROOT_SEEDS:-}" ] && read -r -a ROOT_SEEDS <<<"$MKROOT_SEEDS"
# Only for compiling the samples; unpacked into the stage, never into the root.
BUILD_PKGS=(glibc-devel glibc-static kernel-headers gcc vulkan-headers)

log() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
# APFS clone when possible (no extra space), plain copy otherwise. Like
# `cp -L`: a symlinked source is copied as the file it points to.
clone() { cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"; }

check_tools() {
    local t
    for t in curl tar zstd python3 shasum; do
        command -v "$t" >/dev/null || die "missing $t (brew install zstd)"
    done
}

# ------------------------------------------------------------------- lock
fetch_meta() {   # repo-name base-url -> prints "name=<primary.xml.zst path>=<revision>"
    local name="$1" base="$2" md="$META/$1-repomd.xml"
    curl -fsSL --retry 3 --connect-timeout 20 -o "$md" "$base/repodata/repomd.xml" \
        || die "cannot fetch $base/repodata/repomd.xml"
    local primary href sum rev
    primary="$(python3 - "$md" <<'PY'
import sys, xml.etree.ElementTree as ET
ns = '{http://linux.duke.edu/metadata/repo}'
root = ET.parse(sys.argv[1]).getroot()
for d in root.findall(ns + 'data'):
    if d.get('type') == 'primary':
        ck = d.find(ns + 'checksum')
        assert ck.get('type') == 'sha256', ck.get('type')
        print(d.find(ns + 'location').get('href'), ck.text, root.findtext(ns + 'revision'))
PY
)" || die "cannot parse $md"
    read -r href sum rev <<< "$primary"
    [ -n "$sum" ] || die "no primary metadata in $md"
    local f; f="$META/$(basename "$href")"
    if [ ! -f "$f" ] || [ "$(sha256 "$f")" != "$sum" ]; then
        log "  fetch   $base/$href" >&2
        curl -fsSL --retry 3 --connect-timeout 20 -o "$f.part" "$base/$href"
        [ "$(sha256 "$f.part")" = "$sum" ] || die "$href: sha256 differs from repomd.xml"
        mv "$f.part" "$f"
    fi
    log "  $name: repomd revision $rev, $(basename "$href")" >&2
    printf '%s=%s=%s\n' "$name" "$f" "$rev"
}

do_lock() {
    log "== lock: Fedora 43 aarch64 repodata -> $LOCK"
    mkdir -p "$META"
    local repos=()
    # Later repos win: updates/ holds the newest build of anything updated.
    repos+=("$(fetch_meta releases "$F43_RELEASE")")
    repos+=("$(fetch_meta updates "$F43_UPDATES")")
    python3 - "$LOCK.new" "${ROOT_SEEDS[*]}" "${BUILD_PKGS[*]}" "${repos[@]}" <<'PY'
import datetime, re, subprocess, sys
import xml.etree.ElementTree as ET
C = '{http://linux.duke.edu/metadata/common}'
R = '{http://linux.duke.edu/metadata/rpm}'
out, seeds, builds = sys.argv[1], sys.argv[2].split(), sys.argv[3].split()
pkgs, head = {}, []
for spec in sys.argv[4:]:
    repo, path, rev = spec.split('=')
    head.append(f'#   {repo:9} repomd revision {rev}, {path.rsplit("/", 1)[1]}')
    z = subprocess.Popen(['zstd', '-dcq', path], stdout=subprocess.PIPE)
    for _, el in ET.iterparse(z.stdout):
        if el.tag != C + 'package':
            continue
        arch = el.findtext(C + 'arch')
        if arch in ('aarch64', 'noarch'):
            v, fmt, ck = el.find(C + 'version'), el.find(C + 'format'), el.find(C + 'checksum')
            assert ck.get('type') == 'sha256'
            pkgs[el.findtext(C + 'name')] = dict(
                file=el.find(C + 'location').get('href').rsplit('/', 1)[1],
                epoch=v.get('epoch') or '0', ver=v.get('ver'), rel=v.get('rel'), sha=ck.text,
                size=el.find(C + 'size').get('package'), src=fmt.findtext(R + 'sourcerpm'),
                prov=[e.get('name') for e in fmt.findall(R + 'provides/' + R + 'entry')],
                req=[e.get('name') for e in fmt.findall(R + 'requires/' + R + 'entry')],
                obs=[(e.get('name'), e.get('flags'), e.get('epoch'), e.get('ver'), e.get('rel'))
                     for e in fmt.findall(R + 'obsoletes/' + R + 'entry')])
        el.clear()
    if z.wait():
        sys.exit(f'zstd failed on {path}')

def vercmp(a, b):
    """rpm's rpmvercmp(): digit/letter segments compared in turn, ~ sorts first, ^ after."""
    dig = lambda c: c.isascii() and c.isdigit()
    alp = lambda c: c.isascii() and c.isalpha()
    i = j = 0
    while i < len(a) or j < len(b):
        while i < len(a) and not (dig(a[i]) or alp(a[i])) and a[i] not in '~^': i += 1
        while j < len(b) and not (dig(b[j]) or alp(b[j])) and b[j] not in '~^': j += 1
        ca, cb = a[i:i + 1], b[j:j + 1]
        if ca == '~' or cb == '~':
            if ca != '~': return 1
            if cb != '~': return -1
            i += 1; j += 1; continue
        if ca == '^' or cb == '^':
            if not ca: return -1
            if not cb: return 1
            if ca != '^': return 1
            if cb != '^': return -1
            i += 1; j += 1; continue
        if not (ca and cb):
            break
        seg = dig if dig(ca) else alp
        k, l = i, j
        while k < len(a) and seg(a[k]): k += 1
        while l < len(b) and seg(b[l]): l += 1
        if l == j:                       # digits against letters: digits win
            return 1 if seg is dig else -1
        sa, sb = a[i:k], b[j:l]
        if seg is dig:
            sa, sb = sa.lstrip('0'), sb.lstrip('0')
            if len(sa) != len(sb):
                return 1 if len(sa) > len(sb) else -1
        if sa != sb:
            return 1 if sa > sb else -1
        i, j = k, l
    if i >= len(a) and j >= len(b):
        return 0
    return -1 if i >= len(a) else 1

def satisfies(p, flags, e, v, r):
    """Does package p's own EVR meet an (obsoletes) constraint?"""
    if not flags or v is None:
        return True
    c = (int(p['epoch']) > int(e or 0)) - (int(p['epoch']) < int(e or 0)) or vercmp(p['ver'], v)
    if not c and r is not None:
        c = vercmp(p['rel'], r)
    return {'LT': c < 0, 'LE': c <= 0, 'EQ': c == 0, 'GE': c >= 0, 'GT': c > 0}[flags]

# What dnf does on an upgrade: a package another one obsoletes is replaced by
# it (Fedora 43's tigervnc 1.16 split tigervnc-server-minimal into
# tigervnc-x11-server, which obsoletes and provides the old name).
obsoleted = {}
for name, q in pkgs.items():
    for n, flags, e, v, r in q['obs']:
        if n != name and n in pkgs and satisfies(pkgs[n], flags, e, v, r):
            obsoleted[n] = name
provides = {}
for name, p in pkgs.items():
    for x in p['prov']:
        provides.setdefault(x, set()).add(name)
# A shared-library requirement: "libX11.so.6()(64bit)", "libc.so.6(GLIBC_2.34)(64bit)".
soname = re.compile(r'^[^\s()/]+\.so[^\s()]*\(.*\)\(64bit\)$')
sel, todo = {}, []
for s in seeds:
    via = 'seed'
    if s.startswith('so:'):
        want = s[3:] + '()(64bit)'
        prov = provides.get(want)
        if not prov:
            print(f'  warning: nothing provides {want} (seed)')
            continue
        s = min(prov, key=lambda x: (len(x), x))
        via = f'seed {want}'
    while s in obsoleted:
        print(f'  note: seed {s} is obsoleted by {obsoleted[s]}, taking that')
        via = f'seed {s}, obsoleted by {obsoleted[s]}'
        s = obsoleted[s]
    todo.append((s, via))
while todo:
    name, via = todo.pop(0)
    if name in sel:
        continue
    if name not in pkgs:
        sys.exit(f'no package {name} in the repodata')
    sel[name] = via
    for r in pkgs[name]['req']:
        if not soname.match(r):
            continue
        prov = provides.get(r, set())
        prov = {x for x in prov if x not in obsoleted} or prov
        if not prov:
            print(f'  warning: nothing provides {r} ({name} requires it)')
        elif not prov & (sel.keys() | {t[0] for t in todo}):
            pick = min(prov, key=lambda x: (len(x), x))
            if len(prov) > 1:
                print(f'  note: {r} has providers {sorted(prov)}, took {pick}')
            todo.append((pick, f'{name} {r}'))
rows = [('root', n, sel[n]) for n in sorted(sel)]
for n in builds:
    if n not in pkgs:
        sys.exit(f'no package {n} in the repodata')
    if n not in sel:
        rows.append(('build', n, 'samples'))
with open(out, 'w') as f:
    f.write('# scripts/mkroot-rpm.lock -- written by scripts/mkroot-rpm.sh --relock, not by hand.\n')
    f.write(f'# Fedora 43 aarch64 Everything, resolved {datetime.date.today()} from\n')
    f.write('\n'.join(head) + '\n')
    f.write(f'# root seeds: {" ".join(seeds)} + their shared-library closure\n')
    f.write(f'# build (samples only, not closed over): {" ".join(builds)}\n')
    f.write('# NEVRA = name-epoch:version-release.arch = epoch + file name.\n#\n')
    f.write('# group file epoch sha256 size source-rpm  <- why\n')
    for g, n, via in rows:
        p = pkgs[n]
        f.write(f"{g:5} {p['file']} {p['epoch']} {p['sha']} {p['size']} {p['src']}  <- {via}\n")
tot = sum(int(pkgs[n]['size']) for _, n, _ in rows)
print(f'  {len(sel)} root + {len(rows) - len(sel)} build packages, {tot:,} bytes')
PY
    mv "$LOCK.new" "$LOCK"
}

# Lock lines without comments: group file epoch sha256 size source-rpm
lock_entries() { grep -v '^#' "$LOCK" | awk 'NF >= 6 {print $1, $2, $3, $4, $5, $6}'; }

# ------------------------------------------------------------------ fetch
fetch_rpm() {   # file sha256 source-rpm
    local f="$1" sum="$2" src="$3" dest="$RPMDIR/$1"
    if [ -f "$dest" ] && [ "$(sha256 "$dest")" = "$sum" ]; then
        return 0
    fi
    # Koji path: the source package's name/version/release, the binary's arch.
    local s="${src%.src.rpm}"; local srel="${s##*-}"; s="${s%-*}"
    local sver="${s##*-}" sname="${s%-*}"
    local arch="${f%.rpm}"; arch="${arch##*.}"
    local l; l="$(printf %s "$f" | cut -c1 | tr '[:upper:]' '[:lower:]')"
    # A 404 just means "not in this tree": the next one is tried quietly.
    local url
    for url in "$F43_UPDATES/Packages/$l/$f" "$F43_RELEASE/Packages/$l/$f" \
               "$F43_ARCHIVE_UPDATES/Packages/$l/$f" "$F43_ARCHIVE_RELEASE/Packages/$l/$f" \
               "$KOJI/$sname/$sver/$srel/$arch/$f"; do
        if curl -fsL --retry 3 --connect-timeout 20 -o "$dest.part" "$url"; then
            if [ "$(sha256 "$dest.part")" = "$sum" ]; then
                mv "$dest.part" "$dest"; log "  fetch   $url"; return 0
            fi
            log "  sha256 mismatch: $url"
        fi
    done
    rm -f "$dest.part"
    die "could not fetch $f with sha256 $sum (tried updates, releases, archive, Koji)"
}

do_fetch() {
    log "== fetch: $(lock_entries | wc -l | tr -d ' ') RPMs from $LOCK -> $RPMDIR"
    mkdir -p "$RPMDIR"
    local g f e sum size src n=0
    while read -r g f e sum size src; do
        fetch_rpm "$f" "$sum" "$src"; n=$((n + 1))
    done < <(lock_entries)
    log "  $n RPMs present, sha256 as locked"
}

# ------------------------------------------------------------------ stage
do_stage() {
    local want; want="$(lock_entries | shasum -a 256 | cut -d' ' -f1)"
    if [ -f "$STAGE/.lxrt-stage" ] && [ "$(cat "$STAGE/.lxrt-stage")" = "$want" ]; then
        log "== stage: $STAGE up to date"; return 0
    fi
    log "== stage: unpack -> $STAGE"
    local tmp="$STAGE.tmp" g f e sum size src
    rm -rf "$tmp"; mkdir -p "$tmp"
    # macOS volumes are usually case-insensitive: two payload paths that differ
    # only in case would land on one file. Say so if the package set has any.
    while read -r g f e sum size src; do tar -tf "$RPMDIR/$f"; done < <(lock_entries) \
        | python3 -c '
import sys, collections
seen = collections.defaultdict(set)
for line in sys.stdin:
    p = line.rstrip("\n").lstrip(".").rstrip("/")
    for d in ("/bin/", "/sbin/", "/lib/", "/lib64/"):      # merged /usr
        if p.startswith(d): p = "/usr" + p
    seen[p.lower()].add(p)
clash = [sorted(v) for v in seen.values() if len(v) > 1]
for c in clash: print("  warning: case-insensitive clash:", " ".join(c))
print(f"  {len(seen)} payload paths, {len(clash)} differ only in case")'
    while read -r g f e sum size src; do
        case "$f" in
            # gcc: only the GCC install dir (crt files, libgcc.a, libgcc_eh.a,
            # the libgcc_s.so script); the compiler binaries are of no use here.
            gcc-[0-9]*) tar -xf "$RPMDIR/$f" -C "$tmp" ./usr/lib/gcc ;;
            *) tar -xf "$RPMDIR/$f" -C "$tmp" --exclude './usr/lib/.build-id*' ;;
        esac
    done < <(lock_entries)
    # Merged /usr, as on Fedora (the filesystem package owns these links and
    # is not unpacked). Fold any payload still using the old paths first.
    local d
    for d in bin sbin lib lib64; do
        if [ -d "$tmp/$d" ] && [ ! -L "$tmp/$d" ]; then
            mkdir -p "$tmp/usr/$d"; cp -a "$tmp/$d/." "$tmp/usr/$d/"; rm -rf "${tmp:?}/$d"
        fi
        [ -L "$tmp/$d" ] || ln -s "usr/$d" "$tmp/$d"
    done
    # Absolute symlinks would point into the Mac's own /usr: make them relative.
    python3 - "$tmp" <<'PY'
import os, sys
root, n = sys.argv[1], 0
for dp, dns, fns in os.walk(root):
    for name in dns + fns:
        p = os.path.join(dp, name)
        if os.path.islink(p) and os.readlink(p).startswith('/'):
            t = os.readlink(p)
            os.unlink(p)
            os.symlink(os.path.relpath(os.path.join(root, t.lstrip('/')), dp), p)
            n += 1
print(f'  {n} absolute symlinks made relative')
PY
    [ -e "$tmp/usr/lib/ld-linux-aarch64.so.1" ] && { [ -n "${MKROOT_STAGE_ONLY:-}" ] || [ -x "$tmp/usr/bin/Xvnc" ]; } \
        && [ -e "$tmp/usr/share/X11/xkb/rules/evdev" ] || die "stage incomplete after unpacking"
    echo "$want" > "$tmp/.lxrt-stage"
    rm -rf "$STAGE"; mv "$tmp" "$STAGE"
    log "  $(lock_entries | wc -l | tr -d ' ') RPMs unpacked, $(du -sh "$STAGE" | cut -f1)"
}

# --------------------------------------------------------------- ELF helper
# elfdeps closure <stage> <elf>...  -> "<soname> <path in stage>" for every
#                                      library the ELFs need, transitively
# elfdeps check <root>              -> every DT_NEEDED resolves inside <root>
elfdeps() {
    python3 - "$@" <<'PY'
import os, struct, sys

def elf(path):
    """PT_INTERP, DT_NEEDED and DT_RPATH/RUNPATH of an ELF64 LE file, else None."""
    with open(path, 'rb') as f:
        h = f.read(64)
        if len(h) < 64 or h[:4] != b'\x7fELF' or h[4] != 2 or h[5] != 1:
            return None
        phoff, = struct.unpack_from('<Q', h, 32)
        phentsize, phnum = struct.unpack_from('<HH', h, 54)
        f.seek(phoff)
        ph = f.read(phentsize * phnum)
        loads, dyn, interp = [], None, None
        for i in range(phnum):
            typ, _, off, va, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', ph, i * phentsize)
            if typ == 1:
                loads.append((va, off, filesz))
            elif typ == 2:
                dyn = (off, filesz)
            elif typ == 3:
                f.seek(off)
                interp = f.read(filesz).rstrip(b'\0').decode()
        info = dict(interp=interp, needed=[], rpath=[])
        if not dyn:
            return info
        f.seek(dyn[0])
        d = f.read(dyn[1])
        ents = []
        for i in range(0, len(d) - 15, 16):
            tag, val = struct.unpack_from('<qQ', d, i)
            if tag == 0:
                break
            ents.append((tag, val))
        strva = dict(ents).get(5)
        stroff = next(off + strva - va for va, off, sz in loads if va <= strva < va + sz)
        def s(o):
            f.seek(stroff + o)
            b = b''
            while b'\0' not in b:
                b += f.read(256)
            return b[:b.index(b'\0')].decode()
        for tag, val in ents:
            if tag == 1:
                info['needed'].append(s(val))
            elif tag in (15, 29):
                info['rpath'] += [p for p in s(val).split(':') if p]
        return info

SYSDIRS = ['/lib64', '/usr/lib64']   # glibc's default path on aarch64; no ld.so.cache
LDSO = 'ld-linux-aarch64.so.1'       # satisfied by the already-loaded interpreter

def find(root, name, dirs):
    for d in dirs:
        p = root + d + '/' + name
        if os.path.exists(p):
            return p
    return None

mode = sys.argv[1]
if mode == 'closure':
    stage, todo, seen = sys.argv[2], list(sys.argv[3:]), {}
    while todo:
        f = todo.pop()
        info = elf(f)
        if info is None:
            sys.exit(f'not an ELF64: {f}')
        for n in info['needed']:
            if n == LDSO or n in seen:
                continue
            p = find(stage, n, SYSDIRS)
            if not p:
                sys.exit(f'{f}: needs {n}, not in {stage}')
            seen[n] = p
            todo.append(p)
    for n in sorted(seen):
        print(n, seen[n])
elif mode == 'check':
    root, n_elf, bad = sys.argv[2], 0, []
    for dp, dns, fns in os.walk(root):
        rel = os.path.relpath(dp, root)
        if rel == 'tmp' or rel.startswith('tmp/'):
            dns[:] = []        # runtime state, not part of the build
            continue
        for fn in fns:
            p = os.path.join(dp, fn)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            info = elf(p)
            if info is None:
                continue
            n_elf += 1
            # The emulator prefix must be self-contained: FEX-emu's DT_RPATH
            # names only it, and it is all a bwrap sandbox gets.
            emu = rel.startswith('usr/lib/lxrt-emu')
            dirs = ['/usr/lib/lxrt-emu'] if emu else info['rpath'] + SYSDIRS
            if info['interp'] and not os.path.exists(root + info['interp']):
                bad.append(f'{p}: interpreter {info["interp"]} missing')
            for n in info['needed']:
                if n == LDSO:
                    ok = os.path.exists(root + ('/usr/lib/lxrt-emu/' if emu else '/usr/lib/') + n)
                else:
                    ok = find(root, n, dirs) is not None
                if not ok:
                    bad.append(f'{os.path.relpath(p, root)}: {n} not found')
    for b in bad:
        print('  MISSING ' + b)
    print(f'  {n_elf} ELF files, {"every DT_NEEDED resolves inside the root" if not bad else str(len(bad)) + " unresolved"}')
    sys.exit(1 if bad else 0)
PY
}

# ------------------------------------------------------------------- root
do_root() {
    local out="$1"
    local b
    for b in FEX FEXServer FEXGetConfig FEX-emu; do
        [ -f "$FEX_OUT/$b" ] || die "no $FEX_OUT/$b (run scripts/build-fex-host.sh first)"
    done
    log "== root: $out"
    local new="$out.new.$$"
    rm -rf "$new"
    mkdir -p "$new"/usr/{bin,sbin,lib/lxrt-emu,lib64/dri,share/X11} \
             "$new"/{etc,tmp,dev/shm,var/lib/xkb}
    for b in bin sbin lib lib64; do ln -s "usr/$b" "$new/$b"; done

    # Programs. Xvnc compiles keymaps with popen("xkbcomp ...") through /bin/sh.
    for b in bash Xvnc xkbcomp; do clone "$STAGE/usr/bin/$b" "$new/usr/bin/$b"; done
    ln -s bash "$new/usr/bin/sh"
    # FEX, as the VM scripts named it (Release as FEX-gb, RelWithDebInfo -dbg).
    clone "$FEX_OUT/FEX" "$new/usr/bin/FEX-gb"
    clone "$FEX_OUT/FEXServer" "$new/usr/bin/FEXServer"
    clone "$FEX_OUT/FEXGetConfig" "$new/usr/bin/FEXGetConfig"
    [ -f "$FEX_OUT/FEX-dbg" ] && clone "$FEX_OUT/FEX-dbg" "$new/usr/bin/FEX-gb-dbg"
    [ -f "$FEX_OUT/FEXServer-dbg" ] && clone "$FEX_OUT/FEXServer-dbg" "$new/usr/bin/FEXServer-dbg"

    # Mesa swrast for Xvnc's GLX: the DRI loader shim (swrast_dri.so and
    # libdril_dri.so are one file in Fedora) dlopens libgallium, so neither
    # appears in anyone's DT_NEEDED and both are named here.
    local gallium
    gallium="$(cd "$STAGE/usr/lib64" && ls libgallium-*.so)"
    clone "$STAGE/usr/lib64/dri/swrast_dri.so" "$new/usr/lib64/dri/swrast_dri.so"
    clone "$STAGE/usr/lib64/dri/libdril_dri.so" "$new/usr/lib64/dri/libdril_dri.so"
    clone "$STAGE/usr/lib64/$gallium" "$new/usr/lib64/$gallium"

    # Every library they need, transitively (the VM script's ldd closure),
    # as real files named by soname.
    local libs name src n=0
    libs="$(elfdeps closure "$STAGE" "$new/usr/bin/bash" "$new/usr/bin/Xvnc" "$new/usr/bin/xkbcomp" \
            "$new/usr/lib64/dri/swrast_dri.so" "$new/usr/lib64/$gallium" \
            "$new/usr/bin/FEX-gb" "$new/usr/bin/FEXServer" "$new/usr/bin/FEXGetConfig")"
    while read -r name src; do
        clone "$src" "$new/usr/lib64/$name"; n=$((n + 1))
    done <<< "$libs"
    log "  $n libraries (closure of bash, Xvnc, xkbcomp, swrast, $gallium, FEX)"

    # The loader: Fedora's PT_INTERP says /lib (F43 ships it only there); a
    # copy in lib64 serves images that name /lib64 (as tests/elf/mkroot.sh did).
    clone "$STAGE/usr/lib/ld-linux-aarch64.so.1" "$new/usr/lib/ld-linux-aarch64.so.1"
    clone "$STAGE/usr/lib/ld-linux-aarch64.so.1" "$new/usr/lib64/ld-linux-aarch64.so.1"

    # Emulator prefix: FEX-emu's PT_INTERP and DT_RPATH name /usr/lib/lxrt-emu
    # (scripts/build-fex-host.sh links it that way), and runtime/mounts.c binds
    # this one directory into every bwrap sandbox.
    clone "$FEX_OUT/FEX-emu" "$new/usr/lib/lxrt-emu/FEX"
    clone "$STAGE/usr/lib/ld-linux-aarch64.so.1" "$new/usr/lib/lxrt-emu/ld-linux-aarch64.so.1"
    for b in libc.so.6 libm.so.6 libstdc++.so.6 libgcc_s.so.1; do
        clone "$STAGE/usr/lib64/$b" "$new/usr/lib/lxrt-emu/$b"
    done

    # The runtime's ELF libvulkan.so.1 (make shim; built on the host).
    if [ -f "$REPO/build/libvulkan.so.1" ]; then
        clone "$REPO/build/libvulkan.so.1" "$new/usr/lib64/libvulkan.so.1"
    else
        log "  note: no build/libvulkan.so.1 -- run 'make shim' for the Vulkan tests"
    fi

    # XKB data for Xvnc/xkbcomp. Fedora: /usr/share/X11/xkb -> ../xkeyboard-config-2.
    cp -RL "$STAGE/usr/share/X11/xkb" "$new/usr/share/X11/xkb"

    printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$new/etc/resolv.conf"
    printf '127.0.0.1 localhost\n::1 localhost\n' > "$new/etc/hosts"
    printf 'built by scripts/mkroot-rpm.sh, lock sha256 %s\n' \
        "$(lock_entries | shasum -a 256 | cut -d' ' -f1)" > "$new/.lxrt-root-rpm"

    elfdeps check "$new"
    if [ -d "$out" ]; then
        if [ -d "$out/tmp" ]; then rmdir "$new/tmp"; mv "$out/tmp" "$new/tmp"; fi
        rm -rf "$out"
    fi
    mv "$new" "$out"
    log "  $(find "$out" -path "$out/tmp" -prune -o \( -type f -o -type l \) -print | wc -l | tr -d ' ') files and links, $(du -sh "$out" | cut -f1)"
}

# ---------------------------------------------------------------- samples
# The test programs tests/elf/run.sh runs, built as tests/elf/mkroot.sh built
# them in the VM (same sources, same flags), with clang/lld instead of gcc.
write_sample_sources() {   # dir
    cat > "$1/lxrt_sample.c" <<'CEOF'
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("glibc static-pie alive: argc=%d argv0=%s monotonic=%lld.%09ld\n",
           argc, argv[0], (long long)ts.tv_sec, ts.tv_nsec);
    char buf[64];
    snprintf(buf, sizeof buf, "malloc+stdio+snprintf ok, strlen=%zu", strlen(argv[0]));
    puts(buf);
    return 0;
}
CEOF
    cat > "$1/lxrt_tls.c" <<'CEOF'
#include <stdio.h>
#include <unistd.h>
__thread volatile unsigned long tlsvar;
int main(void) {
    tlsvar = 0xC0FFEEUL;
    printf("TLS escrito: 0x%lx\n", tlsvar);
    for (int i = 0; i < 300; i++) usleep(200);   /* force context switches */
    unsigned long v = tlsvar;
    printf("tras 300 usleep: 0x%lx -> %s\n", v,
           v == 0xC0FFEEUL ? "PRESERVADO" : "*** PERDIDO ***");
    return v == 0xC0FFEEUL ? 0 : 1;
}
CEOF
    cat > "$1/lxrt_pth.c" <<'CEOF'
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#define NTHREADS 4
#define NITER    20000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static long counter; static int ready;
__thread unsigned long tls_id;
static void *worker(void *arg) {
    unsigned long id = (unsigned long)arg;
    tls_id = 0xBEEF0000UL + id;
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NITER; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    return (void *)(tls_id != 0xBEEF0000UL + id);
}
int main(void) {
    pthread_t t[NTHREADS];
    for (unsigned long i = 0; i < NTHREADS; i++)
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) { perror("pthread_create"); return 2; }
    printf("creados %d hilos\n", NTHREADS);
    usleep(50000);
    pthread_mutex_lock(&mu); ready = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    long bad = 0;
    for (int i = 0; i < NTHREADS; i++) { void *r; pthread_join(t[i], &r); bad += (long)r; }
    long want = (long)NTHREADS * NITER;
    printf("counter = %ld (esperado %ld) %s, TLS malo en %ld hilos\n",
           counter, want, counter == want ? "OK" : "*** MAL ***", bad);
    return (counter == want && bad == 0) ? 0 : 1;
}
CEOF
    cat > "$1/lxrt_sig.c" <<'CEOF'
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>
static volatile sig_atomic_t hits, last_sig;
static volatile unsigned long saved_pc;
static volatile long marker;
static void handler(int sig, siginfo_t *si, void *uap) {
    hits++; last_sig = sig;
    ucontext_t *uc = uap;
    saved_pc = uc ? (unsigned long)uc->uc_mcontext.pc : 0;
    marker += 7;
}
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) || sigaction(SIGALRM, &sa, NULL)) { perror("sigaction"); return 2; }
    long before = marker; raise(SIGUSR1);
    printf("1) raise(SIGUSR1): hits=%d sig=%d pc=0x%lx marker+%ld  %s\n", hits, last_sig, saved_pc,
           marker - before, (hits==1 && last_sig==SIGUSR1 && saved_pc && marker-before==7) ? "OK" : "*** MAL ***");
    volatile long a=111,b=222,c=333; raise(SIGUSR1);
    printf("2) contexto tras handler: a=%ld b=%ld c=%ld hits=%d  %s\n", a,b,c,hits,
           (a==111&&b==222&&c==333&&hits==2)?"OK":"*** MAL ***");
    sigset_t set, old; sigemptyset(&set); sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    int bh = hits; raise(SIGUSR1); int during = hits;
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("3) bloqueo: hits durante=%d despues=%d  %s\n", during, hits,
           (during==bh && hits==bh+1)?"OK":"*** MAL ***");
    int h = hits; alarm(1); pause();
    printf("4) SIGALRM asincrono: hits=%d sig=%d  %s\n", hits, last_sig,
           (hits==h+1 && last_sig==SIGALRM)?"OK":"*** MAL ***");
    return 0;
}
CEOF
}

do_samples() {
    local dir="$1"
    [ -x "$LLVM_BIN/clang" ] && [ -x "$LLD" ] || die "samples need Homebrew clang and ld.lld (brew install llvm lld)"
    [ -f "$STAGE/usr/lib64/libc.a" ] || die "no glibc-static in $STAGE"
    log "== samples: clang $("$LLVM_BIN/clang" --version | head -1 | sed 's/.*version //') -> $dir"
    local gccdir; gccdir="$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* | tail -1)"
    local src="$dir/.src" t="$REPO/tests/elf"
    mkdir -p "$dir"; rm -rf "$src"; mkdir -p "$src"
    write_sample_sources "$src"
    local cc=("$LLVM_BIN/clang" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE"
              --gcc-install-dir="$gccdir" -fuse-ld=lld --ld-path="$LLD"
              -I"$REPO/runtime/include" -I"$t" -w)
    build() { log "  $1"; "${cc[@]}" "${@:2}" -o "$dir/$1"; }
    build hello_glibc  -static-pie -O2 "$src/lxrt_sample.c"
    build hello_dyn    -fPIE -pie -O2 "$src/lxrt_sample.c"
    build tls_test     -static-pie -O2 "$src/lxrt_tls.c"
    build pth_test     -static-pie -O2 "$src/lxrt_pth.c" -lpthread
    build fam_test     -static-pie -O2 "$t/syscall_families.c" -lpthread
    build jit_wx       -static-pie -O1 "$t/jit_wx.c"
    build jit_fork     -static-pie -O1 "$t/jit_fork.c"
    build jit_fork_mt  -O1 -fPIE -pie "$t/jit_fork_mt.c" -lpthread
    build x18_test     -static-pie -O1 -pthread "$t/x18_test.c"
    build mremap_test  -static-pie -O2 "$t/mremap_test.c"
    build tfd_sfd_test -static-pie -O2 "$t/tfd_sfd_test.c"
    build posix_timer_test -static-pie -O2 "$t/posix_timer_test.c"
    build inotify_test -static-pie -O2 "$t/inotify_test.c"
    build bwrap_test   -static-pie -O2 "$t/bwrap_test.c"
    build sig_test     -static-pie -O2 "$src/lxrt_sig.c"
    build vk_test      -static-pie -O2 "$t/vk_bridge.c"
    if [ -f "$REPO/build/libvulkan.so.1" ]; then
        # Linked by SONAME against the host-built shim, as in the VM.
        ln -sf "$REPO/build/libvulkan.so.1" "$src/libvulkan.so"
        build vkshim_test    -fPIE -pie -O2 "$t/vk_shim_consumer.c" -L"$src" -lvulkan
        build vkpresent_test -fPIE -pie -O2 "$t/vk_present.c" -L"$src" -lvulkan
        build vktri_test     -fPIE -pie -O2 "$t/vk_triangle.c" -L"$src" -lvulkan
    else
        log "  note: no build/libvulkan.so.1 (make shim): Vulkan shim samples skipped"
    fi
    rm -rf "$src"
    log "  $(ls "$dir" | wc -l | tr -d ' ') programs"
}

step() { local t; t=$(date +%s); "$@"; log "  ($1: $(( $(date +%s) - t )) s)"; }

main() {
    local relock=0
    if [ "${1:-}" = --relock ]; then relock=1; shift; fi
    [ $# -ge 1 ] || [ "$relock" = 1 ] || { sed -n '/^# Usage:/,/^#   \[samples-dir\]/p' "$0" >&2; exit 2; }
    check_tools
    local t0; t0=$(date +%s)
    if [ "$relock" = 1 ] || [ ! -f "$LOCK" ]; then step do_lock; fi
    [ $# -ge 1 ] || return 0
    # Never touch a root this script did not make (e.g. the VM-made one).
    if [ -e "$1" ] && [ ! -f "$1/.lxrt-root-rpm" ]; then
        die "refusing: $1 exists and was not made by this script (move it aside)"
    fi
    step do_fetch
    step do_stage
    # MKROOT_STAGE_ONLY=1: the caller assembles the root from the stage.
    [ -z "${MKROOT_STAGE_ONLY:-}" ] || return 0
    step do_root "$1"
    [ $# -lt 2 ] || step do_samples "$2"
    log "== done in $(( $(date +%s) - t0 )) s: LXRT_ROOT=$1${2:+ LXRT_SAMPLES=$2}"
}
main "$@"
