#!/bin/bash
# scripts/install-native-proton.sh on a synthetic root: it refuses an unknown
# Proton build and unexpected bytes, patches the known instruction once
# (idempotent), links files/bin and installs the tool files; --uninstall
# removes only its own directory. No Steam, no Wine: a fake Proton tree.
set -u
cd "$(dirname "$0")/../.." || exit 1
W=$(mktemp -d "${TMPDIR:-/tmp}/native-proton.XXXXXX")
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { echo "  ok    $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; echo "        $2"; fail=$((fail + 1)); }

ROOT="$W/arm64root"
STEAM="$ROOT/tmp/armhome/.local/share/Steam"
SRC="$STEAM/steamapps/common/Proton Experimental (ARM64)"
mkdir -p "$SRC/files/bin-arm64" "$SRC/files/lib/wine/aarch64-unix" "$STEAM/compatibilitytools.d"
printf '#!/bin/sh\necho fake wine\n' > "$SRC/files/bin-arm64/wine"; chmod 755 "$SRC/files/bin-arm64/wine"
printf '#!/usr/bin/env python3\nprint("fake proton")\n' > "$SRC/proton"
# ntdll.so with the known instruction at the known offset
python3 - "$SRC/files/lib/wine/aarch64-unix/ntdll.so" <<'PY'
import sys
d = bytearray(0x70000)
d[0x62bf0:0x62bf4] = bytes.fromhex("0000b012")
open(sys.argv[1], "wb").write(d)
PY

# 1. unknown build: refused, nothing written
echo "1790000000 experimental-11.0-20260101-arm64" > "$SRC/version"
out=$(STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" 2>&1); rc=$?
if [ "$rc" -ne 0 ] && grep -q "refusing" <<<"$out" && [ ! -e "$STEAM/compatibilitytools.d/steamarm-native-proton" ]; then
    ok "an unknown Proton build is refused"
else
    bad "unknown build refused" "rc=$rc: $out"
fi

# 2. the known build: cloned, patched, linked, tool files installed
echo "1790839316 experimental-11.0-20261001-arm64" > "$SRC/version"
out=$(STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" 2>&1); rc=$?
D="$STEAM/compatibilitytools.d/steamarm-native-proton"
have=$(xxd -s $((0x62bf0)) -l 4 -p "$D/dist/files/lib/wine/aarch64-unix/ntdll.so" 2>/dev/null)
if [ "$rc" -eq 0 ] && [ "$have" = "00008052" ] && [ "$(readlink "$D/dist/files/bin")" = "bin-arm64" ] &&
   [ -x "$D/steamarm-native-proton" ] && [ -f "$D/toolmanifest.vdf" ] && [ -f "$D/compatibilitytool.vdf" ] &&
   [ "$(xxd -s $((0x62bf0)) -l 4 -p "$SRC/files/lib/wine/aarch64-unix/ntdll.so")" = "0000b012" ]; then
    ok "known build: copy patched (source untouched), files/bin linked, tool files installed"
else
    bad "install" "rc=$rc bytes=$have: $out"
fi

# 3. again: idempotent, already patched
out=$(STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" 2>&1); rc=$?
if [ "$rc" -eq 0 ] && grep -q "already patched" <<<"$out"; then
    ok "a second install is idempotent"
else
    bad "idempotent install" "rc=$rc: $out"
fi

# 4. unexpected bytes in the source: refused
python3 - "$SRC/files/lib/wine/aarch64-unix/ntdll.so" <<'PY'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(0x62bf0); f.write(bytes.fromhex("deadbeef"))
PY
rm -rf "$D"
out=$(STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" 2>&1); rc=$?
if [ "$rc" -ne 0 ] && grep -q "refusing" <<<"$out" && [ ! -e "$D" ]; then
    ok "unexpected bytes at the patch offset are refused"
else
    bad "unexpected bytes refused" "rc=$rc: $out"
fi

# 5. uninstall removes only the tool directory
python3 - "$SRC/files/lib/wine/aarch64-unix/ntdll.so" <<'PY'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(0x62bf0); f.write(bytes.fromhex("0000b012"))
PY
STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" >/dev/null 2>&1
out=$(STEAMARM_STATE="$W" scripts/install-native-proton.sh --root "$ROOT" --uninstall 2>&1); rc=$?
if [ "$rc" -eq 0 ] && [ ! -e "$D" ] && [ -x "$SRC/files/bin-arm64/wine" ]; then
    ok "--uninstall removes the tool and leaves Valve's copy"
else
    bad "uninstall" "rc=$rc: $out"
fi

echo "== native_proton: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
