#!/bin/bash
# Windows probes through Proton, no VM: the clocks (tick.c), D3D9 / D3D11 (DXVK) and
# D3D12 (VKD3D-Proton) -> Vulkan -> FEX thunks -> the runtime's Vulkan shim
# -> MoltenVK -> Metal, each presenting cleared frames to a native window.
# The *_32 probes are the same programs built for 32-bit Windows: an i386
# Wine under FEX's 32-bit mode, with the 32-bit Vulkan thunks.
#
#   tests/win/run.sh                 the default set (64-bit and 32-bit)
#   tests/win/run.sh d3d11 d3d9_32   some of them
#
# Needs: the Steam root with Proton Experimental installed, the graphics
# stack in it (scripts/install-steamroot-gfx.sh), and mingw-w64 (Homebrew).
# The native X server (display :2) is started if it is not running.
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=/tmp/lxrt-steamroot
PFX_GUEST=/tmp/fexhome/wtest
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$ROOT/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so" ] ||
    { echo "no Vulkan thunks in the Steam root (scripts/install-steamroot-gfx.sh)" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

# The prefix's DXVK / VKD3D-Proton DLLs, as Proton's own script would copy them.
SYS32="$ROOT$PFX_GUEST/drive_c/windows/system32"
if [ ! -d "$SYS32" ]; then
    DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" wineboot -i >/dev/null 2>&1
fi
cp -f "$PROTON"/lib/wine/dxvk/x86_64-windows/*.dll "$PROTON"/lib/wine/vkd3d-proton/x86_64-windows/*.dll "$SYS32/"
cp -f "$PROTON"/lib/wine/dxvk/i386-windows/*.dll "$PROTON"/lib/wine/vkd3d-proton/i386-windows/*.dll "$SYS32/../syswow64/"

PASS=0 FAIL=0
for t in ${@:-tick tick_32 tone tone_32 regwin d3d11 d3d12 d3d9 d3d9_32 d3d11_32 d3d12_32}; do
    api=${t%_32} cc=x86_64-w64-mingw32-gcc
    [ "$api" = "$t" ] || cc=i686-w64-mingw32-gcc
    exe="$ROOT/tmp/${t}_clear.exe"
    rm -f "$ROOT/tmp/probe-result.txt"
    if [ "$api" = regwin ]; then
        # Registry written in a session with a window, checked after wineserver exits.
        $cc -O2 -mconsole -o "$exe" tests/win/regwin.c -luser32 -ladvapi32 || { FAIL=$((FAIL + 1)); continue; }
        val="v$$$RANDOM"; log="$LOGS/win-$t.log"
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
            scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:\\tmp\\${t}_clear.exe" "$val" > "$log" 2>&1
        for _ in $(seq 1 60); do pgrep -f "[b]in/wineserver" >/dev/null || break; sleep 1; done
        if grep -q "\"$val\"" "$ROOT$PFX_GUEST/user.reg" && ! grep -q "corrupted" "$log"; then
            echo "  ok    $t: value written in a session with a window is in user.reg"; PASS=$((PASS + 1))
        else
            echo "  FAIL  $t (log $log)"; FAIL=$((FAIL + 1))
        fi
        continue
    fi
    if [ "$api" = tone ]; then
        # Sound: a stream from the probe must reach the Mac's PulseAudio.
        $cc -O2 -mconsole -o "$exe" tests/win/tone.c -lwinmm || { FAIL=$((FAIL + 1)); continue; }
        scripts/audio.sh start >/dev/null 2>&1
        log="$LOGS/win-$t.log"; seen=0
        PULSE_SERVER=unix:/tmp/pulse/native \
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
            scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:\\tmp\\${t}_clear.exe" > "$log" 2>&1 &
        pid=$!
        for _ in $(seq 1 400); do
            /opt/homebrew/opt/pulseaudio/bin/pactl -s unix:$ROOT/tmp/pulse/native list sink-inputs 2>/dev/null |
                grep -q "application.name = \"${t}_clear.exe\"" && seen=1
            [ $seen = 1 ] && break
            kill -0 $pid 2>/dev/null || break
            sleep 0.3
        done
        wait $pid 2>/dev/null
        if [ $seen = 1 ] && grep -q "== tone probe: ok" "$log"; then
            echo "  ok    $t: 440 Hz stream reached the Mac's PulseAudio"; PASS=$((PASS + 1))
        else
            echo "  FAIL  $t (log $log)"; FAIL=$((FAIL + 1))
        fi
        continue
    fi
    if [ "$api" = tick ]; then
        $cc -O2 -mconsole -o "$exe" tests/win/tick.c || { FAIL=$((FAIL + 1)); continue; }
    else
        $cc -O2 -mconsole -o "$exe" "tests/win/${api}_clear.c" \
            -l"$api" -ldxgi -luser32 -lgdi32 -luuid -ldxguid || { FAIL=$((FAIL + 1)); continue; }
    fi
    log="$LOGS/win-$t.log"
    DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all \
    WINEDLLOVERRIDES="d3d9,d3d11,d3d12,d3d12core,dxgi,d3d10core=n" \
    LXRT_ROOT=$ROOT FEX_ROOTFS=/ scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:\\tmp\\${t}_clear.exe" > "$log" 2>&1 &
    pid=$!
    # A wall-clock limit that does not rely on the guest honouring SIGALRM.
    for _ in $(seq 1 300); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    if kill -0 $pid 2>/dev/null; then
        echo "        (timeout: stopping the probe)"
        pkill -9 -f "${t}_clear.exe" 2>/dev/null
        pkill -9 -f wineserver 2>/dev/null
        kill -9 $pid 2>/dev/null
    fi
    wait $pid 2>/dev/null
    [ -f "$ROOT/tmp/probe-result.txt" ] && cat "$ROOT/tmp/probe-result.txt" >> "$log"
    if grep -q "== $api probe: ok" "$log"; then
        if [ "$api" = tick ]; then
            echo "  ok    $t: $(grep -h '^ms across' "$log" | head -1)"
        else
            echo "  ok    $t: $(grep -h 'adapter' "$log" | head -1 | sed 's/.*adapter: //'), $(grep -ho '[0-9.]* fps' "$log")"
        fi
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $t (log $log)"; grep -v '^\[lxrt\]\|^[ID] ' "$log" | tail -5 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
done
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
