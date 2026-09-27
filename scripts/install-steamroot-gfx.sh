#!/bin/bash
# Make the Steam root run Windows games the way Steam launches them: through
# Steam Linux Runtime 4.0 (pressure-vessel) and x86 Proton, with Vulkan
# reaching MoltenVK through FEX's library thunks.
#
#   scripts/install-steamroot-gfx.sh        (Steam must not be running)
#
# What it installs into $ROOT (default /tmp/lxrt-steamroot):
#   usr/lib/lxrt-emu/FEX            the current FEX build (FEX-emu variant):
#                                   the one pressure-vessel's containers run
#   usr/lib/lxrt-emu/thunks/        Vulkan host + guest thunk libraries
#   usr/lib/lxrt-emu/libvulkan.so.1 the runtime's Vulkan shim (build/), and
#   usr/lib/lxrt-emu/libX11...      the aarch64 X libraries the host thunk
#                                   dlopens. /usr/lib/lxrt-emu is visible in
#                                   every container (runtime/mounts.c); /opt
#                                   and /usr/lib64 are not.
#   lib64/libvulkan.so.1            the shim, for runs outside a container
#   tmp/fexhome/.fex-emu/AppConfig/steam.json: HideHypervisorBit, Steam client only
#   tmp/fexhome/.fex-emu/AppConfig/steamwebhelper.json: no Vulkan thunks there
#   tmp/fexhome/.fex-emu/Config.json + ThunksDB.json
#                                   Vulkan thunks for every x86-64 process,
#                                   and HideHypervisorBit: with FEX's
#                                   hypervisor CPUID leaves visible, the Steam
#                                   client decides it runs on arm64 and picks
#                                   ARM64 Proton, which cannot work on macOS
#                                   (patches/fex-lxrt-hide-hypervisor.patch).
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}"
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
EMU="$ROOT/usr/lib/lxrt-emu"
THUNKS="$BUILD/thunks-build"
ARMLIBS="${ARMLIBS:-/tmp/lxrt-root/usr/lib64}"
log() { printf '[install-steamroot-gfx] %s\n' "$*"; }
die() { log "error: $*"; exit 1; }

[ -d "$EMU" ] || die "no $EMU (scripts/mksteamroot.sh first)"
if pgrep -f 'build/lxrun .*ubuntu12_32/steam ' >/dev/null; then
    die "Steam is running (scripts/run-steam.sh --stop first)"
fi
for f in "$BUILD/out/FEX-emu" "$THUNKS/HostThunks/libvulkan-host.so" \
         "$THUNKS/GuestThunks/libvulkan-guest.so" build/libvulkan.so.1; do
    [ -f "$f" ] || die "missing $f (build-fex-host.sh emu / build-fex-thunks.sh / make shim)"
done

put() { # src dst: atomic replace, so a running process keeps its old copy
    cp "$1" "$2.new.$$" && mv -f "$2.new.$$" "$2"
}

log "FEX -> $EMU/FEX"
put "$BUILD/out/FEX-emu" "$EMU/FEX"

log "Vulkan thunks -> $EMU/thunks"
mkdir -p "$EMU/thunks/HostThunks" "$EMU/thunks/GuestThunks"
put "$THUNKS/HostThunks/libvulkan-host.so" "$EMU/thunks/HostThunks/libvulkan-host.so"
put "$THUNKS/GuestThunks/libvulkan-guest.so" "$EMU/thunks/GuestThunks/libvulkan-guest.so"
# i386 guests (Proton's 32-bit Wine): FEX looks for <dir>_32.
if [ -f "$THUNKS/HostThunks_32/libvulkan-host.so" ] && [ -f "$THUNKS/GuestThunks_32/libvulkan-guest.so" ]; then
    mkdir -p "$EMU/thunks/HostThunks_32" "$EMU/thunks/GuestThunks_32"
    put "$THUNKS/HostThunks_32/libvulkan-host.so" "$EMU/thunks/HostThunks_32/libvulkan-host.so"
    put "$THUNKS/GuestThunks_32/libvulkan-guest.so" "$EMU/thunks/GuestThunks_32/libvulkan-guest.so"
fi

# The test root shares $HOME/.fex-emu with the Steam root (tmp/fexhome is a
# link), so it needs the thunks at the same path, or every x86-64 process
# there dies on "Requested thunking via guest library ... does not exist".
OTHER="${STEAMARM_TEST_ROOT:-/tmp/lxrt-root}"
if [ -d "$OTHER/usr" ] && [ "$(cd "$OTHER" && pwd -P)" != "$(cd "$ROOT" && pwd -P)" ]; then
    log "Vulkan thunks -> $OTHER/usr/lib/lxrt-emu/thunks (shared FEX config)"
    mkdir -p "$OTHER/usr/lib/lxrt-emu/thunks/HostThunks" "$OTHER/usr/lib/lxrt-emu/thunks/GuestThunks"
    put "$THUNKS/HostThunks/libvulkan-host.so" "$OTHER/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so"
    put "$THUNKS/GuestThunks/libvulkan-guest.so" "$OTHER/usr/lib/lxrt-emu/thunks/GuestThunks/libvulkan-guest.so"
    if [ -f "$THUNKS/GuestThunks_32/libvulkan-guest.so" ]; then
        mkdir -p "$OTHER/usr/lib/lxrt-emu/thunks/HostThunks_32" "$OTHER/usr/lib/lxrt-emu/thunks/GuestThunks_32"
        put "$THUNKS/HostThunks_32/libvulkan-host.so" "$OTHER/usr/lib/lxrt-emu/thunks/HostThunks_32/libvulkan-host.so"
        put "$THUNKS/GuestThunks_32/libvulkan-guest.so" "$OTHER/usr/lib/lxrt-emu/thunks/GuestThunks_32/libvulkan-guest.so"
    fi
fi

log "Vulkan shim -> $EMU and $ROOT/lib64"
put build/libvulkan.so.1 "$EMU/libvulkan.so.1"
put build/libvulkan.so.1 "$ROOT/lib64/libvulkan.so.1"

log "aarch64 X libraries -> $EMU"
for l in libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libXau.so.6 libXdmcp.so.6 \
         libxcb-dri3.so.0 libxcb-present.so.0 libxcb-randr.so.0 libxcb-sync.so.1 libxcb-xfixes.so.0; do
    [ -e "$ARMLIBS/$l" ] || die "no $ARMLIBS/$l"
    cp -L "$ARMLIBS/$l" "$EMU/$l.new.$$" && mv -f "$EMU/$l.new.$$" "$EMU/$l"
done

CFG="$ROOT/tmp/fexhome/.fex-emu"
log "FEX configuration -> $CFG"
mkdir -p "$CFG"
cp "$BUILD/FEX-thunks/Data/ThunksDB.json" "$CFG/ThunksDB.json" 2>/dev/null ||
    cp "$ROOT/opt/vkthunks/config/ThunksDB.json" "$CFG/ThunksDB.json"
cat > "$CFG/Config.json.new" <<'EOF'
{"Config":{"RootFS":"Ubuntu_24_04","ThunkHostLibs":"/usr/lib/lxrt-emu/thunks/HostThunks","ThunkGuestLibs":"/usr/lib/lxrt-emu/thunks/GuestThunks"},"ThunksDB":{"Vulkan":1}}
EOF
mv -f "$CFG/Config.json.new" "$CFG/Config.json"
# The hypervisor leaves are hidden from the Steam client ONLY (FEX's
# per-program config, named after the executable): pressure-vessel detects
# FEX by the same leaves and, without them, builds no interpreter root --
# every steamwebhelper zygote then died on /proc/self/task (MEASURED).
mkdir -p "$CFG/AppConfig"
echo '{"Config":{"HideHypervisorBit":"1"}}' > "$CFG/AppConfig/steam.json"
# No Vulkan thunks in Steam's own web helper: Chromium's GPU process probes
# Vulkan, MoltenVK brings up Objective-C, the zygote then forks, and macOS
# kills the child ("+[NSPlaceholderString initialize] may have been in
# progress ... when fork() was called", MEASURED: "GPU process isn't usable").
echo '{"ThunksDB":{"Vulkan":0}}' > "$CFG/AppConfig/steamwebhelper.json"
log "done"
