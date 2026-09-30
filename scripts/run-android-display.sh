#!/bin/bash
# Android's display stack on the Mac, with no VM: the x86_64 Android 11 root
# (scripts/mkandroidroot.sh --arch x86_64) under SteamARM's FEX, presenting
# through Waydroid's hwcomposer to a Wayland compositor (scripts/run-weston.sh)
# whose output is a macOS window. docs/ANDROID_RUNTIME_ARCHITECTURE.md,
# "Display"; benchmarks/stage27-android-display.txt.
#
#   scripts/run-android-display.sh start    binder, properties, the graphics
#                                           HALs, SurfaceFlinger, bootanimation
#   scripts/run-android-display.sh stop     what start started, nothing else
#   scripts/run-android-display.sh status
#
# What start does, in init's order (there is no init here yet):
#   servicemanager, hwservicemanager, vndservicemanager  (binder contexts)
#   setprop: what Waydroid's container manager writes into /vendor/waydroid.prop
#            -- ro.hardware.gralloc=default (ashmem/memfd buffers, copied to
#            wl_shm by the composer), ro.hardware.egl=swiftshader (GLES on the
#            CPU), waydroid.xdg_runtime_dir / waydroid.wayland_display (the
#            Weston socket), waydroid.active_apps=Waydroid (one window with the
#            whole screen), sys.use_memfd=true
#   the HALs: graphics.allocator@2.0, vendor.waydroid.task@1.0 (the composer
#            waits for it), configstore@1.1, graphics.composer@2.1 (loads
#            hwcomposer.waydroid, which connects to Weston)
#   surfaceflinger as uid system (1000), bootanimation as uid graphics (1003):
#            LXRT_BINDER_UID, what init's `user` line gives them; SurfaceFlinger
#            admits those two uids without asking system_server's permission
#            service, which does not run here
#
# Environment:
#   ANDROID_X86_ROOT      the root (/Volumes/SteamARMAndroid/root-x86_64). Keep
#                         its path short: FEX's server socket under
#                         <root>/data/local/tmp must fit Darwin's 104-byte
#                         sun_path (a 44-byte root path failed, MEASURED)
#   ANDROID_DISPLAY_DIR   host state: private binder hub and property service
#                         directories, PIDs, logcat (/tmp/lxrt-android-display-<uid>),
#                         and <dir>/input: the guests' /dev/input (LXRT_INPUT_DIR,
#                         runtime/evdev.c), where the composer makes its input
#                         FIFOs, not in the shared /tmp/lxrt-input
#   ANDROID_DISPLAY_LOGD  1: run tests/android/logd.py on <root>/dev/socket/logdw
#                         (only if nobody else is bound there)
#   ANDROID_DISPLAY_BOOTANIM  0: no bootanimation
#   WESTON_XDG, WESTON_SOCKET   as scripts/run-weston.sh
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="${ANDROID_X86_ROOT:-/Volumes/SteamARMAndroid/root-x86_64}"
DIR="${ANDROID_DISPLAY_DIR:-/tmp/lxrt-android-display-$(id -u)}"
XDG="${WESTON_XDG:-/dev/shm/steamarm-wayland}"
SOCKET="${WESTON_SOCKET:-wayland-0}"
HOSTSOCK="/tmp/lxrt-shm-$(id -u)${XDG#/dev/shm}/$SOCKET"
export ANDROID_X86_ROOT="$ROOT"
BASE_GENV="LXRT_BINDER_DIR=$DIR/binder LXRT_BINDER_HUB_IDLE=10 LXRT_PROPERTY_DIR=$DIR/props LXRT_PROPERTY_PERSIST=$DIR/props/persistent_properties LXRT_PROPERTY_IDLE=60 LXRT_INPUT_DIR=$DIR/input"
now() { python3 -c 'import time; print("%.2f" % time.time())'; }

# guest [ENV=V...] -- <program> [args]: one guest, in the foreground.
guest() {
    local extra=""
    while [ $# -gt 0 ] && [ "$1" != -- ]; do extra="$extra $1"; shift; done
    shift
    ANDROID_X86_GENV="$BASE_GENV$extra" perl -e 'alarm shift; exec @ARGV' 60 scripts/run-android-x86.sh "$@" 2>/dev/null
}
# daemon <name> [ENV=V...] -- <program> [args]: in the background; the PID
# (run-android-x86.sh execs into lxrun, so $! is the guest) to $DIR/pids.
daemon() {
    local name=$1 extra=""; shift
    while [ $# -gt 0 ] && [ "$1" != -- ]; do extra="$extra $1"; shift; done
    shift
    ANDROID_X86_GENV="$BASE_GENV$extra" scripts/run-android-x86.sh "$@" >"$DIR/$name.log" 2>&1 </dev/null &
    echo "$! $1" > "$DIR/pids/$name"
}
alive() {   # alive <name>: the recorded guest still runs
    local pid prog
    read -r pid prog < "$DIR/pids/$1" 2>/dev/null || return 1
    if [ "$prog" = logd.py ]; then
        ps -p "$pid" -o command= 2>/dev/null | grep -q "logd.py $ROOT"
    else
        ps -p "$pid" -o command= 2>/dev/null | grep -q "lxrun $prog"
    fi
}

case "${1:-status}" in
start)
    [ -x "$ROOT/usr/lib/lxrt-emu/FEX" ] || { echo "run-android-display: no x86_64 root with FEX at $ROOT" >&2; exit 1; }
    [ -S "$HOSTSOCK" ] || { echo "run-android-display: no Wayland socket $XDG/$SOCKET (scripts/run-weston.sh start)" >&2; exit 1; }
    if [ -d "$DIR/pids" ] && [ -n "$(ls "$DIR/pids" 2>/dev/null)" ]; then
        for f in "$DIR"/pids/*; do alive "$(basename "$f")" && { echo "run-android-display: already running ($0 stop first)" >&2; exit 1; }; done
    fi
    mkdir -p "$DIR/binder" "$DIR/props" "$DIR/pids" "$DIR/input" && chmod 700 "$DIR" "$DIR/binder" "$DIR/props"
    rm -f "$DIR"/pids/*
    t0=$(now)
    if [ "${ANDROID_DISPLAY_LOGD:-0}" = 1 ]; then
        if python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.connect(sys.argv[1])' \
               "$ROOT/dev/socket/logdw" 2>/dev/null; then
            echo "run-android-display: someone already listens on $ROOT/dev/socket/logdw; no logcat"
        else
            python3 tests/android/logd.py "$ROOT" --out "$DIR/logcat.txt" >/dev/null 2>&1 &
            echo "$! logd.py" > "$DIR/pids/logd"
        fi
    fi
    daemon servicemanager -- /system/bin/servicemanager
    for ((i = 0; i < 100; i++)); do guest -- /system/bin/service check manager | grep -q found && break; sleep 0.2; done
    daemon hwservicemanager -- /system/bin/hwservicemanager
    daemon vndservicemanager -- /vendor/bin/vndservicemanager /dev/vndbinder
    for ((i = 0; i < 100; i++)); do [ "$(guest -- /system/bin/getprop hwservicemanager.ready)" = true ] && break; sleep 0.2; done
    while read -r k v; do
        guest -- /system/bin/setprop "$k" "$v"
        got=$(guest -- /system/bin/getprop "$k")
        [ "$got" = "$v" ] || echo "run-android-display: $k is '$got', not '$v' (a ro. property keeps its first value)" >&2
    done <<EOF
ro.hardware.gralloc default
ro.hardware.egl swiftshader
sys.use_memfd true
waydroid.xdg_runtime_dir $XDG
waydroid.wayland_display $SOCKET
waydroid.active_apps Waydroid
waydroid.background_start false
EOF
    t_props=$(now)
    daemon allocator -- /vendor/bin/hw/android.hardware.graphics.allocator@2.0-service
    daemon task -- /system/bin/hw/vendor.waydroid.task@1.0-service
    daemon configstore -- /vendor/bin/hw/android.hardware.configstore@1.1-service
    daemon composer -- /vendor/bin/hw/android.hardware.graphics.composer@2.1-service
    daemon surfaceflinger LXRT_BINDER_UID=1000 -- /system/bin/surfaceflinger
    for ((i = 0; i < 150; i++)); do guest -- /system/bin/service check SurfaceFlinger | grep -q found && break; sleep 0.2; done
    t_sf=$(now)
    if [ "${ANDROID_DISPLAY_BOOTANIM:-1}" = 1 ]; then
        daemon bootanimation LXRT_BINDER_UID=1003 -- /system/bin/bootanimation
    fi
    python3 - "$t0" "$t_props" "$t_sf" <<'PY'
import sys
t0, tp, ts = map(float, sys.argv[1:])
print(f"run-android-display: binder and properties up in {tp - t0:.1f} s; SurfaceFlinger registered {ts - t0:.1f} s after start")
PY
    for f in "$DIR"/pids/*; do
        n=$(basename "$f")
        # run-android-x86.sh becomes lxrun only once it has set up (exec).
        for ((i = 0; i < 50; i++)); do alive "$n" && break; sleep 0.1; done
        alive "$n" && echo "  $n pid $(cut -d' ' -f1 "$f")" || echo "  $n NOT RUNNING ($DIR/$n.log)"
    done ;;
stop)
    [ -d "$DIR/pids" ] || { echo "run-android-display: nothing started"; exit 0; }
    # Reverse order: clients before the services they use.
    for n in bootanimation surfaceflinger composer configstore task allocator vndservicemanager hwservicemanager servicemanager logd; do
        [ -f "$DIR/pids/$n" ] || continue
        read -r pid prog < "$DIR/pids/$n"
        alive "$n" && kill "$pid"
        rm -f "$DIR/pids/$n"
    done
    # hwcomposer.waydroid makes its input FIFOs (/dev/input/wl_*_events) as
    # it finds a pointer and a keyboard: in $DIR/input, this stack's own
    # /dev/input (LXRT_INPUT_DIR). Remove them with the stack.
    for f in "$DIR"/input/wl_keyboard_events "$DIR"/input/wl_pointer_events \
             "$DIR"/input/wl_touch_events "$DIR"/input/wl_tablet_events; do
        [ -p "$f" ] && rm -f "$f"
    done
    echo "run-android-display: stopped (the binder hub, property service and FEXServer leave by themselves when idle)" ;;
status)
    [ -n "$(ls "$DIR/pids" 2>/dev/null)" ] || { echo "run-android-display: not started"; exit 3; }
    for f in "$DIR"/pids/*; do [ -e "$f" ] || continue; n=$(basename "$f"); alive "$n" && echo "  $n running" || echo "  $n gone"; done ;;
*)
    sed -n '2,40p' "$0"; exit 2 ;;
esac
