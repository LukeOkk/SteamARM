#!/bin/bash
# steamarm-wlmac (tools/wlmac), the native macOS Wayland compositor:
#  1. an aarch64 Wayland client under lxrun (tests/android/wl_shm_client.c)
#     draws 60 frames into a window; the compositor's dump of that window
#     has the client's four colours in roughly equal parts;
#  2. its globals, as the client lists them;
#  3. input: with --selftest-input the compositor feeds a mouse move, a
#     click and the A key to the first window, and a host wire client
#     (tests/wlmac/input_client.py) must get wl_pointer enter, motion,
#     BTN_LEFT and wl_keyboard KEY_A (30).
# A macOS window appears for a few seconds. No screen capture is used.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
PASS=0 FAIL=0
ok()  { echo "  ok    $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
echo "== tests/wlmac (native Wayland compositor)"
tools/wlmac/build.sh >/dev/null || { bad "build steamarm-wlmac"; exit 1; }
WR="${WESTON_ROOT:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/westonroot}"
xdg=/dev/shm/steamarm-wlmac-test-$$
host="/tmp/lxrt-shm-$(id -u)${xdg#/dev/shm}"
dump=$(mktemp -d /tmp/wlmac-dump.XXXXXX)
stop() { WLMAC_XDG=$xdg scripts/run-wlmac.sh stop >/dev/null 2>&1; }
trap 'stop; rm -rf "$dump" "$host"' EXIT

if [ -d "$WR/tmp" ] && [ -x build/lxrun ]; then
    cflags="-O2 -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -static-pie -fPIE -fuse-ld=lld"
    # shellcheck disable=SC2086
    /opt/homebrew/opt/llvm/bin/clang --target=aarch64-linux-gnu $cflags -o build/wl_shm_client.aarch64 \
        tests/android/wl_shm_client.c 2>/dev/null || { bad "build wl_shm_client"; exit 1; }
    cp build/wl_shm_client.aarch64 "$WR/tmp/wlmac_client"
    WLMAC_XDG=$xdg scripts/run-wlmac.sh start --dump-dir "$dump" >/dev/null || { bad "start steamarm-wlmac"; exit 1; }
    client() { LXRT_ROOT="$WR" XDG_RUNTIME_DIR=$xdg WAYLAND_DISPLAY=wayland-0 build/lxrun /tmp/wlmac_client "$@" 2>&1 | grep -v '^\[lxrt'; }
    out=$(client -n 60 -t wlmac-test -H 1500)
    fps=$(grep -o '[0-9.]* fps' <<<"$out")
    png=$(ls "$dump"/*.png 2>/dev/null | head -1)
    if [ -n "$fps" ] && [ -n "$png" ] && colours=$(python3 tests/wlmac/check_png.py "$png" 2>&1); then
        ok "an aarch64 client under lxrun drew 60 frames ($fps) into its own macOS window; $colours"
    else bad "frames into a window" "$(tail -2 <<<"$out" | tr '\n' ' ') png='$png' $colours"; fi
    # What hwcomposer.waydroid needs of a compositor (no clipboard yet).
    gl=$(client -g | grep -oE '(wl|xdg|wp)_[a-z_]+' | tr '\n' ' ')
    missing=""
    for w in wl_compositor wl_subcompositor wl_shm wl_output wl_seat xdg_wm_base wp_viewporter; do
        grep -qw "$w" <<<"$gl" || missing="$missing $w"
    done
    [ -z "$missing" ] && ok "globals: $gl" || bad "globals" "missing:$missing (listed: $gl)"
    stop
    rm -f "$WR/tmp/wlmac_client"
else
    echo "  skip  frames from a guest client (no Weston root at $WR or no build/lxrun)"
fi

WLMAC_XDG=$xdg scripts/run-wlmac.sh start --selftest-input >/dev/null || { bad "start steamarm-wlmac --selftest-input"; exit 1; }
if out=$(python3 tests/wlmac/input_client.py "$host/wayland-0" 2>&1); then ok "$out"
else bad "input" "$(tail -3 <<<"$out" | tr '\n' ' ')"; fi
stop

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
