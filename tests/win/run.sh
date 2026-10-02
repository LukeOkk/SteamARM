#!/bin/bash
# Windows probes through Proton, no VM: the clocks (tick.c), D3D9 / D3D11 (DXVK) and
# D3D12 (VKD3D-Proton) -> Vulkan -> FEX thunks -> the runtime's Vulkan shim
# -> MoltenVK -> Metal, each presenting cleared frames to a native window.
# The *_32 probes are the same programs built for 32-bit Windows: an i386
# Wine under FEX's 32-bit mode, with the 32-bit Vulkan thunks. The *_wined3d
# probes use Wine's own D3D (WineD3D) on OpenGL from Mesa's Zink, which draws
# with the same Vulkan thunk.
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
# Wine's own vkd3d, which wined3d.dll imports (WineD3D runs below).
cp -f "$PROTON"/lib/vkd3d/x86_64-windows/*.dll "$SYS32/"
cp -f "$PROTON"/lib/vkd3d/i386-windows/*.dll "$SYS32/../syswow64/"

PASS=0 FAIL=0
for t in ${@:-tick tick_32 tone tone_32 regwin d3d11 d3d12 d3d9 d3d9_32 d3d11_32 d3d12_32 d3d9_wined3d d3d11_wined3d d3d9_32_wined3d modeset modeset_fsr modeset_nearest modeset_metalfx sync sync_fsync sync_ntsync}; do
    # <api>_wined3d: Wine's builtin D3D on OpenGL from Mesa's Zink, on the same
    # Vulkan thunk (the launcher's "OpenGL (WineD3D)"; stage38-opengl-zink).
    wined3d=0
    case $t in *_wined3d) wined3d=1 ;; esac
    base=${t%_wined3d}
    # modeset_<filter>: the scaling filter of shim/scaler.c (LXRT_VK_SCALER).
    scaler=""
    case $base in modeset_*) scaler=${base#modeset_}; base=modeset ;; sync_*) base=sync ;; esac
    api=${base%_32} cc=x86_64-w64-mingw32-gcc
    [ "$api" = "$base" ] || cc=i686-w64-mingw32-gcc
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
    if [ "$api" = sync ]; then
        # Windows synchronization (tests/win/sync.c) under each backend the
        # launcher offers: sync (wineserver), sync_fsync (WINEFSYNC=1:
        # futex_waitv.c), sync_ntsync (LXRT_NTSYNC=1: /dev/ntsync, ntsync.c).
        # The server is stopped first: it keeps the backend it started with.
        $cc -O2 -mconsole -o "$exe" tests/win/sync.c || { FAIL=$((FAIL + 1)); continue; }
        pkill -9 -f "[b]in/wineserver" 2>/dev/null
        for _ in $(seq 1 30); do pgrep -f "[b]in/wineserver" >/dev/null || break; sleep 1; done
        case $t in
            sync_fsync)  senv=(WINEFSYNC=1) ;;
            sync_ntsync) senv=(LXRT_NTSYNC=1) ;;
            *)           senv=(PROTON_NO_NTSYNC=1) ;;
        esac
        log="$LOGS/win-$t.log"
        env DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all "${senv[@]}" \
            LXRT_ROOT=$ROOT FEX_ROOTFS=/ scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:\\tmp\\${t}_clear.exe" > "$log" 2>&1 &
        pid=$!
        for _ in $(seq 1 120); do kill -0 $pid 2>/dev/null || break; sleep 1; done
        kill -0 $pid 2>/dev/null && { pkill -9 -f "${t}_clear.exe"; kill -9 $pid; }
        wait $pid 2>/dev/null
        pkill -9 -f "[b]in/wineserver" 2>/dev/null
        mode="wineserver"
        grep -q "fsync: up and running" "$log" && mode="fsync"
        grep -q "ntsync: up and running" "$log" && mode="ntsync"
        want=${t#sync}; want=${want#_}; want=${want:-wineserver}
        if grep -q "== sync probe: ok" "$log" && [ "$mode" = "$want" ]; then
            echo "  ok    $t: $mode, $(grep -h '^event ping-pong' "$log" | sed 's/^event ping-pong: //')"; PASS=$((PASS + 1))
        else
            echo "  FAIL  $t (backend $mode, wanted $want; log $log)"
            grep -v '^\[lxrt\]' "$log" | grep -E "ping-pong|wait all|abandoned|semaphore|== sync" | sed 's/^/        /'
            FAIL=$((FAIL + 1))
        fi
        continue
    fi
    if [ "$api" = modeset ]; then
        # "Escala de resolución": Wine's display-mode emulation (EmulateModeset,
        # scripts/wine-prefix-options.py) in this prefix for this run. The
        # program asks 1280x720 full screen; it must draw at 1280x720 while its
        # X window covers the whole screen (Wine stretches it).
        $cc -O2 -mconsole -o "$exe" tests/win/modeset.c -ld3d11 -ldxgi -luser32 -lgdi32 -luuid -ldxguid ||
            { FAIL=$((FAIL + 1)); continue; }
        reg="$ROOT$PFX_GUEST/user.reg"
        for _ in $(seq 1 60); do pgrep -f "[b]in/wineserver" >/dev/null || break; sleep 1; done
        python3 scripts/wine-prefix-options.py emulate-modeset on "$reg" >/dev/null
        log="$LOGS/win-$t.log"
        env DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG="${WIN_DEBUG:--all}" LXRT_VK_DEBUG=1 \
            LXRT_VK_SCALER="$scaler" LXRT_VK_SCALER_PROBE=1 WINE_DISABLE_FULLSCREEN_HACK=1 LXRT_VK_PADDED_FULLSCREEN=1 \
            WINEDLLOVERRIDES="d3d9,d3d11,d3d12,d3d12core,dxgi,d3d10core=n" \
            LXRT_ROOT=$ROOT FEX_ROOTFS=/ scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:\\tmp\\${t}_clear.exe" > "$log" 2>&1 &
        pid=$!
        geo=""
        for _ in $(seq 1 120); do
            g=$(DISPLAY=:2 xwininfo -root -tree 2>/dev/null | grep '"SteamARM modeset probe"' | grep -oE '[0-9]+x[0-9]+\+' | head -1)
            [ -n "$g" ] && geo=${g%+}
            kill -0 $pid 2>/dev/null || break
            sleep 0.5
        done
        wait $pid 2>/dev/null
        pkill -9 -f wineserver 2>/dev/null
        for _ in $(seq 1 60); do pgrep -f "[b]in/wineserver" >/dev/null || break; sleep 1; done
        python3 scripts/wine-prefix-options.py emulate-modeset off "$reg" >/dev/null
        screen=$(DISPLAY=:2 xdpyinfo 2>/dev/null | awk '/dimensions:/ {print $2}')
        # What reaches the window (the shim's probe, LXRT_VK_SCALER_PROBE),
        # with Proton's fullscreen hack off and the game shown on its
        # full-screen toplevel (LXRT_VK_PADDED_FULLSCREEN, settings-env.py):
        # red in the left half, blue in the right one (B8G8R8A8: 0,0,255 and
        # 255,0,0). With a filter of the shim's own, the image its pass wrote
        # at the window's size; without one, the game's 1280x720 image, which
        # MoltenVK stretches ("stretched") over a surface larger than it.
        px=$(grep -m1 "^\[shim\] probe " "$log")
        what=${scaler:-plain}
        # Another driver than MoltenVK (STEAMARM_VK_ICD=kosmickrisp) has no
        # present scaling and no Metal objects to export: the shim's
        # bilinear blit stands for "plain" and for MetalFX (scaler.c).
        case "${STEAMARM_VK_ICD:-}:$what" in
            ?*:plain|?*:metalfx) what=linear-blit ;;
        esac
        pw=$(echo "$px" | sed -n 's/.* \([0-9]*\)x\([0-9]*\) format.*/\1/p')
        ph=$(echo "$px" | sed -n 's/.* \([0-9]*\)x\([0-9]*\) format.*/\2/p')
        sw=$(grep -m1 "^\[shim\] vkCreateSwapchainKHR extent" "$log" | sed -n 's/.*surface \([0-9]*\)x.*/\1/p')
        filtered=1
        note="; ${px#*\[shim\] probe }"
        echo "$px" | grep -q "probe $what " || filtered=0
        if [ -n "$scaler" ] || [ "$what" = linear-blit ]; then
            [ "${pw:-0}" -gt 1280 ] && [ "${ph:-0}" -gt 720 ] || filtered=0
        else
            [ "${sw:-0}" -gt 1280 ] && grep -q "stretched" "$log" || filtered=0
        fi
        echo "$px" | grep -qE "left [0-3],[0-3],(25[2-5]),255 right (25[2-5]),[0-3],[0-3],255" || filtered=0
        if [ $filtered = 1 ] && grep -q "== modeset probe: ok" "$log" && grep -q "^backbuffer 1280x720" "$log" && [ "$geo" = "$screen" ]; then
            echo "  ok    $t: 1280x720 drawn, X window $geo (the screen); $(grep -o '[0-9]* frames' "$log" | tail -1)$note"; PASS=$((PASS + 1))
        else
            echo "  FAIL  $t (X window '$geo', screen $screen; log $log)"; FAIL=$((FAIL + 1))
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
    if [ $wined3d = 1 ]; then
        # As scripts/settings-env.py sets it: OpenGL 4.5 (stage 39).
        dll=(WINEDLLOVERRIDES="d3d9,d3d11,dxgi,d3d10core=b" GALLIUM_DRIVER=zink
             "MESA_EXTENSION_OVERRIDE=+GL_ARB_vertex_type_2_10_10_10_rev +GL_ARB_texture_buffer_object_rgb32")
    else
        dll=(WINEDLLOVERRIDES="d3d9,d3d11,d3d12,d3d12core,dxgi,d3d10core=n")
    fi
    env DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all "${dll[@]}" \
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
# Nothing of the test prefix stays behind: its services (services.exe,
# winedevice, rpcss...) outlived the suite and kept running for hours.
for p in $(pgrep -f "build/lxrun /tmp/lxrt-root/usr/bin/FEX-gb"); do
    ps eww -o command= -p "$p" 2>/dev/null | grep -q "WINEPREFIX=$PFX_GUEST" && kill -9 "$p" 2>/dev/null
done
pkill -9 -f "build/lxrun .*wineserver" 2>/dev/null
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
