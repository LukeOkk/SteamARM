#!/usr/bin/env bash
# `scripts/steamframe-image.py inventory` must never print a private pacman
# mirror: a device image can point its repositories at a per-device path with
# an access token (the Steam Frame 0.3.0 image does). Needs only python3, so
# it runs on macOS as well as Linux (run.sh needs mkfs.btrfs).
#   tests/steamframe_image/redact.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
TOOL="$PWD/scripts/steamframe-image.py"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok()  { echo "  ok    $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; fail=$((fail + 1)); }

# A fake token: a 64-hex path segment on the SteamOS package host, the shape
# of the image's real mirrors.
TOKEN="$(printf '0123456789abcdef%.0s' 1 2 3 4)"
R="$W/root"
mkdir -p "$R/etc/pacman.d"
cat > "$R/etc/pacman.conf" <<EOF
[options]
Architecture = auto
[core]
Server = https://holo-packages.steamos.cloud/archlinux-test_${TOKEN}_x/pipeline/1/\$repo/os/\$arch
[extra]
Include = /etc/pacman.d/private
[holo-core-aarch64-preview]
Server = https://holo-packages.steamos.cloud/holo-core-aarch64-preview/\$repo/os/\$arch
Server = https://mirror.invalid/\$repo/os/\$arch
EOF
echo "Server = https://sub.steamos.cloud/${TOKEN}/\$repo" > "$R/etc/pacman.d/private"

python3 "$TOOL" inventory "$R" --json "$W/inv.json" --md "$W/inv.md" >/dev/null
for f in inv.json inv.md; do
    if grep -q "$TOKEN" "$W/$f"; then bad "$f leaks a private mirror"; else ok "$f has no private mirror"; fi
done
grep -q 'https://holo-packages.steamos.cloud/<private path redacted>' "$W/inv.md" \
    && grep -q 'https://sub.steamos.cloud/<private path redacted>' "$W/inv.md" \
    && ok "redacted mirrors keep their host (Server= and Include=)" || bad "redacted form"
grep -q 'holo-packages.steamos.cloud/holo-core-aarch64-preview/holo-core-aarch64-preview/os/aarch64' "$W/inv.md" \
    && grep -q 'mirror.invalid/holo-core-aarch64-preview/os/aarch64' "$W/inv.md" \
    && ok "public mirrors stay as they are" || bad "public mirrors"

# The marker rule, in memory only: the URL is built from the script's own
# constant and never written anywhere.
python3 - "$TOOL" <<'PY' && ok "a URL with the do-not-share marker is redacted, any host" || bad "marker rule"
import importlib.util, sys
spec = importlib.util.spec_from_file_location('sfi', sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
mark = m.PRIVATE_URL_MARK
cases = {
    'https://mirror.invalid/a_' + mark.lower() + '_b/core': 'https://mirror.invalid/<private path redacted>',
    'https://' + mark.lower() + '.invalid/core': '<private URL redacted>',
    'https://mirror.invalid/core/os/aarch64': 'https://mirror.invalid/core/os/aarch64',
}
sys.exit(0 if all(m.redact_url(u) == want for u, want in cases.items()) else 1)
PY

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
