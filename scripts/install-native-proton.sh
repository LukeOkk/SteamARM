#!/bin/bash
# Install "SteamARM: Proton ARM64 nativo (experimental)", the compatibility
# tool that runs a Windows game's x86-64 code through FEX's ARM64EC JIT inside
# a native ARM64 Wine (benchmarks/stage57-wine-arm64-native.txt), into the
# SteamARM client's library.
#
#   scripts/install-native-proton.sh [--root arm64root|DIR] [--dry-run]
#   scripts/install-native-proton.sh --uninstall [--root ...]
#
# Source: the client's own download of Valve's "Proton Experimental (ARM64)"
# (steamapps/common; install it from Steam's compatibility list first). A copy
# (an APFS clone: no extra space) goes to compatibilitytools.d/
# steamarm-native-proton/dist with the two changes macOS needs:
#
#   1. ntdll.so's thread block below 2 GiB (virtual_alloc_first_teb's
#      zero_bits 0x7fffffff, for WoW64) must be allowed anywhere: macOS has
#      nothing below 4 GiB. One instruction in the copy, `mov w0, #0x7fffffff`
#      -> `mov w0, #0`, at the offset of the build this was measured with;
#      the bytes are checked first and any other build is refused (a Wine
#      source change replaces this when SteamARM builds its own Wine).
#   2. files/bin -> bin-arm64: Wine execs its server as lib/wine/../../bin/
#      wineserver, and Proton ARM64 ships bin-arm64 (its python script points
#      at it; Wine's own loader does not).
#
# Writes only <root>/tmp/armhome/.local/share/Steam/compatibilitytools.d/
# steamarm-native-proton. Idempotent. The client lists the tool after a
# restart.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ROOT="$STATE/arm64root"
DRY=0 UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --root)
            case "${2:?--root needs arm64root or a directory}" in
                arm64root) ROOT="$STATE/arm64root" ;;
                /*) ROOT="$2" ;;
                *) ROOT="$STATE/$2" ;;
            esac
            shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        *) echo "usage: $0 [--root arm64root|DIR] [--dry-run] [--uninstall]" >&2; exit 2 ;;
    esac
done
log() { printf '[install-native-proton] %s\n' "$*"; }

STEAM_ARM="$ROOT/tmp/armhome/.local/share/Steam"
DEST="$STEAM_ARM/compatibilitytools.d/steamarm-native-proton"
if [ "$UNINSTALL" = 1 ]; then
    [ "$DRY" = 1 ] && { log "would remove $DEST"; exit 0; }
    rm -rf "$DEST"
    log "removed $DEST"
    exit 0
fi

# The build this was measured with: its version file and the instruction's
# bytes at the offset (benchmarks/stage57 §2).
KNOWN_VERSION="experimental-11.0-20261001-arm64"
NTDLL_OFF=$((0x62bf0))
NTDLL_OLD="0000b012"   # mov w0, #0x7fffffff
NTDLL_NEW="00008052"   # mov w0, #0

SRC=""
for name in "Proton Experimental (ARM64)" "Proton 11.0 (ARM64)"; do
    d="$STEAM_ARM/steamapps/common/$name"
    if [ -x "$d/files/bin-arm64/wine" ] && [ -f "$d/proton" ]; then
        SRC="$d"
        break
    fi
done
[ -n "$SRC" ] || { log "no Proton ARM64 in $STEAM_ARM/steamapps/common: install \"Proton Experimental (ARM64)\" from Steam's compatibility list first"; exit 1; }
ver=$(awk 'NR==1 {print $2}' "$SRC/version" 2>/dev/null || true)
if [ "$ver" != "$KNOWN_VERSION" ]; then
    log "refusing: $SRC is '$ver', the thread-block patch is known for '$KNOWN_VERSION' only"
    exit 1
fi
ntdll="$SRC/files/lib/wine/aarch64-unix/ntdll.so"
have=$(xxd -s "$NTDLL_OFF" -l 4 -p "$ntdll" 2>/dev/null || true)
if [ "$have" != "$NTDLL_OLD" ] && [ "$have" != "$NTDLL_NEW" ]; then
    log "refusing: $ntdll has $have at $NTDLL_OFF, not the known instruction $NTDLL_OLD"
    exit 1
fi

if [ "$DRY" = 1 ]; then
    log "would clone $SRC to $DEST/dist, patch ntdll.so at $NTDLL_OFF, link files/bin -> bin-arm64, install the tool files"
    exit 0
fi

mkdir -p "$DEST"
if [ ! -f "$DEST/dist/version" ] || [ "$(awk 'NR==1 {print $2}' "$DEST/dist/version")" != "$ver" ]; then
    log "cloning $SRC (APFS clone)"
    rm -rf "$DEST/dist"
    cp -Rc "$SRC" "$DEST/dist"
fi
target="$DEST/dist/files/lib/wine/aarch64-unix/ntdll.so"
chmod u+w "$target"
python3 - "$target" "$NTDLL_OFF" "$NTDLL_OLD" "$NTDLL_NEW" <<'PY'
import sys
path, off, old, new = sys.argv[1], int(sys.argv[2]), bytes.fromhex(sys.argv[3]), bytes.fromhex(sys.argv[4])
with open(path, "r+b") as f:
    f.seek(off)
    have = f.read(4)
    if have == new:
        print("[install-native-proton] ntdll.so already patched")
    elif have == old:
        f.seek(off)
        f.write(new)
        print("[install-native-proton] ntdll.so: thread block may live above 2 GiB (mov w0, #0 at %#x)" % off)
    else:
        print("[install-native-proton] unexpected bytes %s at %#x" % (have.hex(), off))
        sys.exit(1)
PY
ln -sfn bin-arm64 "$DEST/dist/files/bin"
for f in steamarm-native-proton toolmanifest.vdf compatibilitytool.vdf; do
    cp -f "tools/steamarm-native-proton/$f" "$DEST/$f"
done
chmod 755 "$DEST/steamarm-native-proton"
log "installed $DEST (Proton $ver)"
log "restart the client so that it rereads compatibilitytools.d; pick the tool in a game's Properties > Compatibility"
