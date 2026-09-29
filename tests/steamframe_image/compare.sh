#!/usr/bin/env bash
# `scripts/steamframe-image.py compare`: image packages against pacman
# repository databases. Needs only python3 (the databases here are xz and
# gzip, as holo-core-aarch64-preview publishes them), so it runs on macOS as
# well as Linux; run.sh has the btrfs part and needs mkfs.btrfs. No network.
#   tests/steamframe_image/compare.sh
#
# What it guards (each was wrong or missing before 0.3.5+):
#   - which side is newer, by pacman's vercmp: string order says 1.5b > 1.5
#     and 10 < 9; the old compare only said "differs"
#   - a pkgrel-only difference (a rebuild) told apart from a version change
#   - a package renamed between the two (sdl2 / sdl2-compat,
#     holo-glibc-locales / glibc-locales) named through PROVIDES/REPLACES
#     instead of two unrelated "only" rows
#   - the report records each database's sha256, and names a database by its
#     base name or a redacted URL, never a home path or a private mirror
#   - an extracted root works as well as an inventory JSON, and an inventory
#     written before PROVIDES/REPLACES were recorded still compares
set -euo pipefail
cd "$(dirname "$0")/../.."
TOOL="$PWD/scripts/steamframe-image.py"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { echo "  ok    $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; fail=$((fail + 1)); }

echo "== vercmp (pacman's test/util/vercmptest.sh cases, both directions)"
python3 - "$TOOL" <<'PY' && ok "vercmp agrees with pacman on 49 pairs" || bad "vercmp"
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sf', sys.argv[1])
sf = importlib.util.module_from_spec(spec); spec.loader.exec_module(sf)
cases = '''
1.5.0 1.5.0 0
1.5.1 1.5.0 1
1.5.1 1.5 1
1.5.0-1 1.5.0-1 0
1.5.0-1 1.5.0-2 -1
1.5.0-1 1.5.1-1 -1
1.5.0-2 1.5.1-1 -1
1.5-1 1.5.1-1 -1
1.5-2 1.5.1-1 -1
1.5-2 1.5.1-2 -1
1.5 1.5-1 0
1.5-1 1.5 0
1.1-1 1.1 0
1.0-1 1.1 -1
1.1-1 1.0 1
1.5b-1 1.5-1 -1
1.5b 1.5 -1
1.5b-1 1.5 -1
1.5b 1.5.1 -1
1.0a 1.0alpha -1
1.0alpha 1.0b -1
1.0b 1.0beta -1
1.0beta 1.0rc -1
1.0rc 1.0 -1
1.5.a 1.5 1
1.5.b 1.5.a 1
1.5.1 1.5.b 1
1.5.b-1 1.5.b 0
1.5-1 1.5.b -1
2.0 2_0 0
2.0_a 2_0.a 0
2.0a 2.0.a -1
2___a 2_a 1
0:1.0 0:1.0 0
0:1.0 0:1.1 -1
1:1.0 0:1.0 1
1:1.0 0:1.1 1
1:1.0 2:1.1 -1
1:1.0 0:1.0-1 1
1:1.0-1 0:1.1-1 1
0:1.0 1.0 0
0:1.0 1.1 -1
0:1.1 1.0 1
1:1.0 1.0 1
1:1.0 1.1 1
1:1.1 1.1 1
10 9 1
2.39-2 2.42+r33+gde1fe81f4714-1 -1
1:1.6.8-1.2 1:1.4.9-1 1
'''
bad = []
for line in cases.split('\n'):
    if not line.strip():
        continue
    a, b, want = line.split()
    want = int(want)
    for x, y, w in ((a, b, want), (b, a, -want)):
        got = sf.vercmp(x, y)
        if got != w:
            bad.append(f'vercmp {x} {y} = {got}, want {w}')
for x in bad:
    print('    ' + x)
sys.exit(1 if bad else 0)
PY

echo "== compare: an extracted root against two databases (xz, gzip)"
R="$W/root"
python3 - "$R" "$W" <<'PY'
import io, os, sys, tarfile
root, w = sys.argv[1], sys.argv[2]

def desc(name, version, arch='aarch64', provides=(), replaces=()):
    t = f'%NAME%\n{name}\n\n%VERSION%\n{version}\n\n%ARCH%\n{arch}\n\n'
    if provides:
        t += '%PROVIDES%\n' + '\n'.join(provides) + '\n\n'
    if replaces:
        t += '%REPLACES%\n' + '\n'.join(replaces) + '\n\n'
    return t

image = [
    desc('glibc', '2.39-2'),                                   # repo newer (version)
    desc('pipewire', '1:1.6.8-1.2'),                           # image newer (version, epoch)
    desc('xorg-xwayland', '24.1.9-1.2'),                       # image newer (pkgrel only)
    desc('nss', '3.117-1'),                                    # same
    desc('foo', '1.5b-1'),                                     # repo newer: 1.5b < 1.5
    desc('sdl2', '2.30.2-1'),                                  # repo has sdl2-compat (provides sdl2)
    desc('holo-glibc-locales', '2.39-2', provides=['glibc-locales=2.39-2'],
         replaces=['glibc-locales']),                          # repo has glibc-locales
    desc('deckard-thing', '1-1'),                              # image only, no counterpart
    desc('kio', '6.14.0-1'),                                   # image only; kio5 replaces kio<5.111 only
    desc('bubblewrap', '0.9.0-1'),                             # on both sides: no counterpart needed
    desc('oldlib', '1.0-1'),                                   # newlib replaces oldlib<2
    desc('deckard-mesa', '26.3.0-1', provides=['mesa', 'opengl-driver']),  # unversioned PROVIDES
]
local = os.path.join(root, 'usr/lib/holo/pacmandb/local')
for t in image:
    name, ver = t.split('\n')[1], t.split('\n')[4]
    d = os.path.join(local, f'{name}-{ver}')
    os.makedirs(d)
    open(os.path.join(d, 'desc'), 'w').write(t)
os.makedirs(os.path.join(root, 'etc'))
open(os.path.join(root, 'etc/os-release'), 'w').write('ID=steamos\n')

def db(path, mode, entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode=mode) as tf:
        for t in entries:
            name, ver = t.split('\n')[1], t.split('\n')[4]
            data = t.encode()
            ti = tarfile.TarInfo(f'{name}-{ver}/desc'); ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    open(path, 'wb').write(buf.getvalue())

db(os.path.join(w, 'core.db'), 'w:xz', [
    desc('glibc', '2.42+r33+gde1fe81f4714-1'),
    desc('glibc-locales', '2.42+r33+gde1fe81f4714-1'),
    desc('nss', '3.117-1'),
    desc('pipewire', '1:1.4.9-1'),
])
db(os.path.join(w, 'extra.db'), 'w:gz', [
    desc('nss', '9.9-1'),                                      # shadowed: core comes first
    desc('xorg-xwayland', '24.1.9-1'),
    desc('foo', '1.5-1'),
    desc('sdl2-compat', '2.32.58-1.2', provides=['sdl2=2.32.58']),
    desc('mesa', '1:25.2.7-1'),                                # the image's deckard-mesa provides it
    desc('wine', '10.19-1'),                                   # repo only, no counterpart
    desc('kio5', '5.116.0-2', replaces=['kio<5.111']),         # versioned: not KF6's kio
    desc('bubblewrap', '0.11.0-1'),
    desc('bubblewrap-suid', '0.11.0-1', provides=['bubblewrap=0.11.0-1']),
    desc('newlib', '3.0-1', replaces=['oldlib<2']),
])
PY
out="$W/cmp.md"
python3 "$TOOL" compare "$R" "$W/core.db" "$W/extra.db" --md "$out" --json "$W/cmp.json" >"$W/cmp.log" 2>&1 \
    && ok "compare accepts an extracted root (exit 0)" || { bad "compare on a root"; cat "$W/cmp.log"; }
row() { grep -F -- "| $1 |" "$out" | head -1; }
expect_row() {  # package, the rest of the row as it must read
    case "$(row "$1")" in "| $1 | $2 |") ok "$1: $3" ;; *) bad "$1: $3"; echo "    got:  $(row "$1")"; echo "    want: | $1 | $2 |" ;; esac
}
expect_row glibc '2.39-2 | 2.42+r33+gde1fe81f4714-1 | core | aarch64 | differs | repo | version' "the repository is newer"
expect_row pipewire '1:1.6.8-1.2 | 1:1.4.9-1 | core | aarch64 | differs | image | version' "the image is newer (epoch kept)"
expect_row xorg-xwayland '24.1.9-1.2 | 24.1.9-1 | extra | aarch64 | differs | image | pkgrel' "a rebuild is pkgrel only"
expect_row foo '1.5b-1 | 1.5-1 | extra | aarch64 | differs | repo | version' "1.5b is older than 1.5"
expect_row sdl2 '2.30.2-1 |  |  | aarch64 | image-only |  | repo: sdl2-compat 2.32.58-1.2 (provides this)' "renamed: the repository's package provides it"
expect_row holo-glibc-locales '2.39-2 |  |  | aarch64 | image-only |  | repo: glibc-locales 2.42+r33+gde1fe81f4714-1 (provided by this)' "renamed: it provides the repository's name"
expect_row glibc-locales ' | 2.42+r33+gde1fe81f4714-1 | core | aarch64 | repo-only |  | image: holo-glibc-locales 2.39-2 (provides this)' "the same pair from the repository's side"
expect_row deckard-thing '1-1 |  |  | aarch64 | image-only |  | ' "image only, no counterpart"
expect_row wine ' | 10.19-1 | extra | aarch64 | repo-only |  | ' "repository only, no counterpart"
expect_row deckard-mesa '26.3.0-1 |  |  | aarch64 | image-only |  | repo: mesa 1:25.2.7-1 (provided by this)' "an unversioned PROVIDES (mesa)"
expect_row mesa ' | 1:25.2.7-1 | extra | aarch64 | repo-only |  | image: deckard-mesa 26.3.0-1 (provides this)' "the same pair from the repository's side"
expect_row kio5 ' | 5.116.0-2 | extra | aarch64 | repo-only |  | ' "a versioned REPLACES (kio<5.111) does not match kio 6.14"
expect_row oldlib '1.0-1 |  |  | aarch64 | image-only |  | repo: newlib 3.0-1 (replaces this)' "a versioned REPLACES whose range holds the image's version"
expect_row bubblewrap-suid ' | 0.11.0-1 | extra | aarch64 | repo-only |  | ' "no counterpart for a name both sides have"
grep -qF '| nss |' "$out" && bad "a same-version package is listed without --all" || ok "same versions are counted, not listed"
grep -qF 'differs 5, image-only 6, repo-only 7, same 1' "$out" && ok "counts" || { bad "counts"; grep -E '^(differs|same)' "$out"; }
grep -qF 'differs: image newer 2, repository newer 3, same order 0; pkgrel only 1, version 4. image-only with a repository counterpart (PROVIDES/REPLACES) 4; repo-only with an image counterpart 4.' "$out" \
    && ok "difference breakdown" || { bad "difference breakdown"; grep '^differs:' "$out"; }
python3 - "$W" "$out" <<'PY' && ok "each database is recorded by base name with its sha256; no directory in the report" || bad "sources"
import hashlib, os, sys
w, md = sys.argv[1], open(sys.argv[2]).read()
want = [f"- `{r}`: `{r}.db`, {n} packages, {os.path.getsize(os.path.join(w, r + '.db'))} bytes, sha256 `"
        + hashlib.sha256(open(os.path.join(w, r + '.db'), 'rb').read()).hexdigest() + '`'
        for r, n in (('core', 4), ('extra', 10))]
sys.exit(0 if all(x in md for x in want) and w not in md else 1)
PY
python3 - "$W/cmp.json" <<'PY' && ok "--json has every row, the counts and the sources" || bad "--json"
import json, sys
j = json.load(open(sys.argv[1]))
rows = {r['package']: r for r in j['rows']}
sys.exit(0 if len(rows) == 19 and rows['nss']['state'] == 'same' and rows['nss']['repo'] == 'core'
         and j['counts']['differs:pkgrel'] == 1 and [s['repo'] for s in j['repositories']] == ['core', 'extra']
         else 1)
PY
python3 "$TOOL" compare "$R" "$W/core.db" "$W/extra.db" --states repo-only > "$W/only.md"
grep -qF '| mesa |' "$W/only.md" && ! grep -qF '| glibc |' "$W/only.md" && ok "--states lists only the states asked for" || bad "--states"
python3 "$TOOL" compare "$R" "$W/core.db" --states nonsense >/dev/null 2>&1 && bad "an unknown state is accepted" || ok "an unknown state is refused"

echo "== compare: an inventory JSON, including one written before PROVIDES/REPLACES"
python3 "$TOOL" inventory "$R" --json "$W/inv.json" --md "$W/inv.md" >/dev/null
python3 "$TOOL" compare "$W/inv.json" "$W/core.db" "$W/extra.db" --md "$W/cmp2.md" >/dev/null
diff <(sed 1,4d "$out") <(sed 1,4d "$W/cmp2.md") >/dev/null && ok "inventory JSON and root give the same report" || bad "JSON vs root"
python3 - "$W/inv.json" "$W/old.json" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
for p in j['packages']:
    for k in ('provides', 'replaces', 'builddate'):
        p.pop(k, None)
json.dump(j, open(sys.argv[2], 'w'))
PY
python3 "$TOOL" compare "$W/old.json" "$W/core.db" "$W/extra.db" --md "$W/cmp3.md" >/dev/null \
    && grep -qF '| sdl2 | 2.30.2-1 |  |  | aarch64 | image-only |  | repo: sdl2-compat 2.32.58-1.2 (provides this) |' "$W/cmp3.md" \
    && ok "an old inventory still compares (the repository side still names counterparts)" || bad "old inventory"

echo "== how a database source is named"
python3 - "$TOOL" <<'PY' && ok "URLs as redact_url prints them, files by base name" || bad "source_label"
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sf', sys.argv[1])
sf = importlib.util.module_from_spec(spec); spec.loader.exec_module(sf)
token = '0123456789abcdef' * 4
cases = {
    'https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/core/os/aarch64/core.db':
        'https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/core/os/aarch64/core.db',
    f'https://holo-packages.steamos.cloud/x_{token}_y/core/os/aarch64/core.db':
        'https://holo-packages.steamos.cloud/<private path redacted>',
    '/home/someone/dl/extra.db': 'extra.db',
}
sys.exit(0 if all(sf.source_label(u) == want for u, want in cases.items()) else 1)
PY

echo "== zstd database"
if python3 -c 'import zstandard' 2>/dev/null || command -v zstd >/dev/null; then
    python3 - "$TOOL" "$W/core.db" <<'PY' && ok "a zstd database decodes (module, or the zstd command without it)" || bad "zstd database"
import importlib.util, lzma, shutil, subprocess, sys
spec = importlib.util.spec_from_file_location('sf', sys.argv[1])
sf = importlib.util.module_from_spec(spec); spec.loader.exec_module(sf)
tar = lzma.decompress(open(sys.argv[2], 'rb').read())
try:
    import zstandard
    raw = zstandard.ZstdCompressor().compress(tar)
except ImportError:
    raw = subprocess.run(['zstd', '-q', '-c'], input=tar, capture_output=True, check=True).stdout
want = sf.parse_repo_db(open(sys.argv[2], 'rb').read())
ok = sf.parse_repo_db(raw) == want
if shutil.which('zstd'):
    sf._ZSTD = False                       # as on a Mac without the module
    ok = ok and sf.parse_repo_db(raw) == want
sys.exit(0 if ok and len(want) == 4 else 1)
PY
else
    echo "  skip  no zstandard module and no zstd command"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
