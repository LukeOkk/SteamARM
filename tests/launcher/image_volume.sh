#!/bin/bash
# scripts/image-volume.sh with a real (sparse, nearly empty) disk image: ensure
# grows it to the exact size once, two ensures at once attach it once and
# read-write, and an image attached read-only is attached again read-write.
# macOS only; skipped where hdiutil cannot make or attach images.
#
#   tests/launcher/image_volume.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
[ "$(uname -s)" = Darwin ] || { echo "SKIP: not macOS"; exit 0; }
# The real path: mount(8) names /private/var/..., not /var/....
W="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/image-volume-test.XXXXXX")" && pwd -P)"
B="$W/iv.sparsebundle"
M="$W/mnt"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok    $1"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $1"; }
# The /dev/disk entries of this test's image (hdiutil info lists every image).
disks() {
    hdiutil info 2>/dev/null | awk -v img="$B" '
        /^image-path/ { cur = substr($0, index($0, ":") + 2) }
        /^\/dev\/disk[0-9]+[ \t]/ && cur == img { print $1 }' | sort -u
}
detach_all() { local d; for d in $(disks); do hdiutil detach -force -quiet "$d" 2>/dev/null; done; }
cleanup() { [ -n "${holder:-}" ] && kill "$holder" 2>/dev/null; detach_all; rm -rf "$W"; }
trap cleanup EXIT
mounts() { mount | grep -c " on $M (" ; }
ro() { diskutil info "$M" 2>/dev/null | grep -q -E '^ *(Media|Volume) Read-Only: *Yes'; }
size() { diskutil info "$M" 2>/dev/null | sed -n 's/^ *Disk Size:.*(\([0-9]*\) Bytes).*/\1/p' | head -1; }
# The attachment: hdiutil info's "process ID" of this image, a new one per
# attach (disk numbers are reused, so they cannot tell a detach + attach).
dev() {
    hdiutil info 2>/dev/null | awk -v img="$B" '
        /^image-path/ { cur = substr($0, index($0, ":") + 2) }
        cur == img && /^process ID/ { print $NF; exit }'
}

hdiutil create -quiet -size 100m -type SPARSEBUNDLE -fs "Case-sensitive APFS" -volname IVTest "$B" 2>/dev/null ||
    { echo "SKIP: hdiutil cannot make an image here"; exit 0; }
mkdir -p "$M" "$W/state"
hdiutil attach -quiet -nobrowse -mountpoint "$M" "$B" 2>/dev/null && hdiutil detach -quiet "$M" 2>/dev/null ||
    { echo "SKIP: hdiutil cannot attach an image here"; exit 0; }
# A target just under 21 GiB, as a Mac's disk is 1862.8 GiB: the image grows
# to 20 GiB. diskutil's decimal "20g" (18.6 GiB) would be more than 1 GiB
# short, as "1862g" was (1734 GiB), and the old script grew it again at every
# ensure. The mounted volume is the image less its partition map (and on the
# Mac an EFI partition: 1861.8 GiB of 1862), so a check on the mounted size
# found it "smaller" than the target less 1 GiB and detached it every time.
GIB=1073741824
export STEAMARM_STATE="$W/state" STEAMARM_IMAGE_TARGET_BYTES=$((21 * GIB - 20480))
iv() { scripts/image-volume.sh "$@"; }

echo "== a detached image is grown to the size asked, once"
if iv ensure "$B" "$M" 2>"$W/e1" && [ "$(mounts)" = 1 ]; then ok "ensure attached it"
else bad "ensure did not attach it"; cat "$W/e1"; echo "$pass passed, $fail failed"; exit 1; fi
s=$(size)
[ -n "$s" ] && [ "$s" -gt $((19 * GIB)) ] && ok "20 GiB: $(awk -v b="$s" 'BEGIN { printf "%.2f", b / 1073741824 }') GiB" || bad "size ${s:-unknown} (decimal GB?)"
d1=$(dev)
iv ensure "$B" "$M" 2>"$W/e2"
[ -n "$d1" ] && [ "$(dev)" = "$d1" ] && ! grep -q growing "$W/e2" && ok "second ensure: left attached, not grown again" ||
    { bad "second ensure detached or grew it"; cat "$W/e2"; }
touch "$M/held" 2>/dev/null && ok "read-write" || bad "not writable"

echo "== two ensures at once (the launcher's and run-app.sh's)"
detach_all
iv ensure "$B" "$M" 2>"$W/e3" &
iv ensure "$B" "$M" 2>"$W/e4" &
wait
[ "$(disks | wc -l | tr -d ' ')" -ge 1 ] && [ "$(mounts)" = 1 ] && ok "attached once" ||
    { bad "mounts: $(mounts)"; cat "$W/e3" "$W/e4"; }
! ro && ok "read-write after both" || bad "read-only after two ensures at once"
! grep -q -i -E 'could not|read-only' "$W/e3" "$W/e4" && ok "no error from either" || { bad "errors"; cat "$W/e3" "$W/e4"; }

echo "== an image attached read-only is attached again read-write"
detach_all
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$M" "$B" 2>/dev/null && ro ||
    { echo "SKIP: cannot attach read-only here"; echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]; exit; }
iv ensure "$B" "$M" 2>"$W/e5"
[ "$(mounts)" = 1 ] && ! ro && ok "ensure: read-write now" || { bad "still read-only"; cat "$W/e5"; }

echo "== ... but not while something has a file open on it: ensure says so"
detach_all
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$M" "$B" 2>/dev/null
sleep 300 <"$M/held" &
holder=$!
sleep 0.5
if iv ensure "$B" "$M" 2>"$W/e6"; then bad "ensure succeeded on a read-only image in use"
else ok "ensure fails"; fi
grep -q 'READ-ONLY' "$W/e6" && ok "and names the reason" || { bad "message"; cat "$W/e6"; }
[ "$(mounts)" = 1 ] && ok "the image in use stays attached" || bad "mounts: $(mounts)"
kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null; holder=""

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
