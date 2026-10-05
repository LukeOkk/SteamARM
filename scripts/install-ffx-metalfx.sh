#!/bin/bash
# Install SteamARM's FidelityFX API DLLs (tools/ffx-metalfx, built by
# make ffx-metalfx) into the x86 Steam root, where the game tools point Wine
# at them for a game's FSR 3.1 / FSR 4 -> MetalFX (STEAMARM_WIN_UPSCALER=metalfx,
# launcher "Escalado del juego"; games with an anti-cheat are left alone).
#
#   scripts/install-ffx-metalfx.sh [--root DIR] [--dry-run]
#   scripts/install-ffx-metalfx.sh --uninstall [--root DIR]
#
# What it writes, all under <root>/opt/steamarm/wine (and nothing else):
#   x86_64-windows/amd_fidelityfx_dx12.dll, amd_fidelityfx_vk.dll,
#   amd_fidelityfx_upscaler_dx12.dll   the builtins (WINEDLLPATH=/opt/steamarm/wine)
#   anticheat-appids.txt               games never given the DLLs, by Steam app id
#                                      (tools/ffx-metalfx/anticheat-appids.txt)
#   steamarm-ffx                       the x86 client's launch-option wrapper:
#                                      tools/steamarm-fex-proton/steamarm-fex-proton
#                                      under that name ("/opt/steamarm/wine/steamarm-ffx
#                                      %command%" in a game's launch options)
# Never Proton's own files: a Proton update would drop them, and every game
# would get them. Nothing changes for a game until the variable is set and the
# game tool adds the DLL override (tools/steamarm-fex-proton). Files are
# replaced by rename, so a running game keeps the copy it loaded; Steam may
# be running. scripts/setup.sh runs this at every setup (step "steam").
#
# --root     the x86 Steam root (default $LXRT_ROOT, else /tmp/lxrt-steamroot)
# FFX_BUILD  the built DLLs' directory (default build/ffx-metalfx, made with
#            make ffx-metalfx first; set, nothing is built: tests)
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}" DRY=0 UNINSTALL=0
NAMES="amd_fidelityfx_dx12 amd_fidelityfx_vk amd_fidelityfx_upscaler_dx12"
while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:?--root needs a directory}"; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
log() { echo "install-ffx-metalfx: $*"; }
die() { log "error: $*" >&2; exit 1; }

[ -d "$ROOT/usr" ] || die "no x86 Steam root at $ROOT (scripts/setup.sh roots)"
DEST="$ROOT/opt/steamarm/wine"
if [ "$UNINSTALL" = 1 ]; then
    [ -d "$DEST" ] || { log "not installed: $DEST"; exit 0; }
    [ "$DRY" = 1 ] && { log "would remove $DEST"; exit 0; }
    rm -rf -- "$DEST"
    rmdir "$ROOT/opt/steamarm" 2>/dev/null || true
    log "removed $DEST"
    exit 0
fi

SRC="${FFX_BUILD:-}"
if [ -z "$SRC" ]; then
    SRC=build/ffx-metalfx
    if ! make -s ffx-metalfx; then
        # A packaged release carries the DLLs: without mingw-w64 they stay usable.
        for n in $NAMES; do
            [ -f "$SRC/x86_64-windows/$n.dll" ] || die "could not build $SRC/x86_64-windows/$n.dll (make ffx-metalfx; needs mingw-w64)"
        done
        log "warning: make ffx-metalfx failed; installing the DLLs already in $SRC"
    fi
fi
for n in $NAMES; do
    [ -f "$SRC/x86_64-windows/$n.dll" ] || die "missing $SRC/x86_64-windows/$n.dll (make ffx-metalfx)"
done
if [ "$DRY" = 1 ]; then
    log "would install $NAMES (.dll), anticheat-appids.txt and steamarm-ffx into $DEST"
    exit 0
fi

put() {   # put SRC DST MODE: atomic replace
    cp "$1" "$2.new.$$" && chmod "$3" "$2.new.$$" && mv -f "$2.new.$$" "$2"
}
mkdir -p "$DEST/x86_64-windows"
for n in $NAMES; do
    put "$SRC/x86_64-windows/$n.dll" "$DEST/x86_64-windows/$n.dll" 0644
done
put tools/ffx-metalfx/anticheat-appids.txt "$DEST/anticheat-appids.txt" 0644
put tools/steamarm-fex-proton/steamarm-fex-proton "$DEST/steamarm-ffx" 0755
log "installed into $DEST: $NAMES (.dll), anticheat-appids.txt, steamarm-ffx"
